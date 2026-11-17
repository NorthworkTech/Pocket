import { describe, expect, it, beforeEach } from "vitest";
import { openMemoryDatabase, closeDatabase, run, get } from "../src/db/index.js";
import { createApp } from "../src/app.js";
import { getOrCreateUser, createSession, upsertSubscriptionMirror } from "../src/lib/auth.js";
import { sha256Hex } from "../src/lib/crypto.js";

type App = ReturnType<typeof createApp>;

function linkDevice(userId: string, deviceId: string, token: string) {
  run(
    `INSERT INTO device_links (id, user_id, device_id, device_name, linked_at, last_seen_at, device_token_hash)
     VALUES (?, ?, ?, 'Pocket', ?, NULL, ?)`,
    [`link-${deviceId}`, userId, deviceId, new Date().toISOString(), sha256Hex(token)],
  );
}

function entitle(userId: string) {
  upsertSubscriptionMirror(userId, {
    status: "trialing",
    trial_ends_at: new Date(Date.now() + 7 * 86400000).toISOString(),
  });
}

async function sync(app: App, token: string, body: unknown) {
  const res = await app.request("http://localhost/v1/device/sync", {
    method: "POST",
    headers: { authorization: `Bearer ${token}`, "content-type": "application/json" },
    body: JSON.stringify(body),
  });
  return { status: res.status, body: (await res.json()) as any };
}

const iso = (msAgo: number) => new Date(Date.now() - msAgo).toISOString();
const HOUR = 3600_000;

describe("POST /v1/device/sync", () => {
  let app: App;
  let userId: string;
  const TOKEN = "tok-device-a";

  beforeEach(async () => {
    closeDatabase();
    await openMemoryDatabase();
    app = createApp();
    userId = getOrCreateUser("sync@example.com").id;
    linkDevice(userId, "dev-a", TOKEN);
  });

  it("rejects unknown device tokens", async () => {
    const res = await sync(app, "nope", { notes: [] });
    expect(res.status).toBe(401);
  });

  it("returns 402 without Pocket Cloud", async () => {
    const res = await sync(app, TOKEN, { notes: [] });
    expect(res.status).toBe(402);
    expect(res.body.entitled).toBe(false);
    expect(res.body.message).toBe("Pocket Cloud is needed to sync notes and lists.");
  });

  it("inserts notes and lists from the device", async () => {
    entitle(userId);
    const t = iso(HOUR);
    const res = await sync(app, TOKEN, {
      notes: [{ id: "n1", title: "Hello", body: "World", created_at: t, updated_at: t, deleted_at: null }],
      lists: [
        {
          id: "l1",
          title: "Groceries",
          items: [
            { id: "i1", text: "milk", checked: false, updated_at: t },
            { id: "i2", text: "bad", checked: "no", updated_at: t },
            { text: "no id", checked: true, updated_at: t },
          ],
          created_at: t,
          updated_at: t,
          deleted_at: null,
        },
      ],
    });
    expect(res.status).toBe(200);
    expect(typeof res.body.server_time).toBe("string");
    expect(res.body.notes).toHaveLength(1);
    expect(res.body.notes[0]).toMatchObject({
      id: "n1",
      title: "Hello",
      body: "World",
      updated_at: t,
      updated_by_device_id: "dev-a",
      deleted_at: null,
      version: 1,
    });
    expect(res.body.lists[0].items).toEqual([{ id: "i1", text: "milk", checked: false, updated_at: t }]);
  });

  it("device update loses to a newer cloud edit", async () => {
    entitle(userId);
    const session = createSession(userId);
    const cloudEdit = iso(HOUR);
    const create = await app.request("http://localhost/v1/notes", {
      method: "POST",
      headers: { cookie: `pocket_session=${session.token}`, "content-type": "application/json" },
      body: JSON.stringify({ id: "n1", title: "From phone", body: "new", updated_at: cloudEdit }),
    });
    expect(create.status).toBe(201);

    const res = await sync(app, TOKEN, {
      notes: [{ id: "n1", title: "Stale device", body: "old", updated_at: iso(2 * HOUR) }],
    });
    expect(res.status).toBe(200);
    expect(res.body.notes[0]).toMatchObject({ id: "n1", title: "From phone", version: 1 });

    const newer = await sync(app, TOKEN, {
      notes: [{ id: "n1", title: "Fresh device", body: "x", updated_at: iso(0) }],
    });
    expect(newer.body.notes[0]).toMatchObject({ title: "Fresh device", version: 2 });
  });

  it("device tombstone deletes and stays visible in the response", async () => {
    entitle(userId);
    await sync(app, TOKEN, { notes: [{ id: "n1", title: "a", body: "", updated_at: iso(2 * HOUR) }] });
    const deletedAt = iso(HOUR);
    const res = await sync(app, TOKEN, {
      notes: [{ id: "n1", title: "a", body: "", updated_at: iso(2 * HOUR), deleted_at: deletedAt }],
    });
    expect(res.status).toBe(200);
    expect(res.body.notes).toHaveLength(1);
    expect(res.body.notes[0].deleted_at).toBe(deletedAt);

    // Companion no longer sees it.
    const session = createSession(userId);
    const list = await app.request("http://localhost/v1/notes", {
      headers: { cookie: `pocket_session=${session.token}` },
    });
    expect((await list.json()).notes).toHaveLength(0);

    // Tombstones older than 30 days drop out of the sync response.
    run("UPDATE cloud_notes SET deleted_at = ? WHERE id = 'n1'", [iso(31 * 24 * HOUR)]);
    const later = await sync(app, TOKEN, {});
    expect(later.body.notes).toHaveLength(0);
  });

  it("skips ids that belong to another user", async () => {
    entitle(userId);
    const other = getOrCreateUser("other@example.com");
    entitle(other.id);
    linkDevice(other.id, "dev-b", "tok-device-b");
    await sync(app, "tok-device-b", {
      notes: [{ id: "shared-id", title: "Theirs", body: "", updated_at: iso(HOUR) }],
    });

    const res = await sync(app, TOKEN, {
      notes: [
        { id: "shared-id", title: "Hijack", body: "", updated_at: iso(0) },
        { id: "mine", title: "Mine", body: "", updated_at: iso(0) },
      ],
    });
    expect(res.status).toBe(200);
    expect(res.body.notes.map((n: { id: string }) => n.id)).toEqual(["mine"]);
    expect(res.body.skipped.notes).toEqual([{ id: "shared-id", reason: "unavailable" }]);
    const row = get<{ title: string; user_id: string }>(
      "SELECT title, user_id FROM cloud_notes WHERE id = 'shared-id'",
    );
    expect(row).toEqual({ title: "Theirs", user_id: other.id });
  });

  it("clamps future clocks and enforces size limits", async () => {
    entitle(userId);
    const res = await sync(app, TOKEN, {
      notes: [
        { id: "future", title: "t".repeat(400), body: "", updated_at: "2099-01-01T00:00:00.000Z" },
        { id: "big", title: "big", body: "x".repeat(200 * 1024 + 1), updated_at: iso(0) },
      ],
    });
    expect(res.status).toBe(200);
    expect(res.body.notes).toHaveLength(1);
    expect(Date.parse(res.body.notes[0].updated_at)).toBeLessThanOrEqual(Date.now());
    expect(res.body.notes[0].title).toHaveLength(300);
    expect(res.body.skipped.notes).toEqual([{ id: "big", reason: "too_large" }]);

    const tooMany = await sync(app, TOKEN, {
      notes: Array.from({ length: 1001 }, (_, i) => ({ id: `n${i}` })),
    });
    expect(tooMany.status).toBe(413);
  });

  it("records sd_present via sync and via the attest heartbeat", async () => {
    entitle(userId);
    const sdOf = () =>
      get<{ sd_present: number }>("SELECT sd_present FROM device_links WHERE device_id = 'dev-a'")!
        .sd_present;
    expect(sdOf()).toBe(0);

    const res = await sync(app, TOKEN, { sd_present: true });
    expect(res.body.sd_present).toBe(true);
    expect(sdOf()).toBe(1);

    // Omitted sd_present leaves it alone.
    expect((await sync(app, TOKEN, {})).body.sd_present).toBe(true);

    const attest = (headers: Record<string, string>, q = "") =>
      app.request(`http://localhost/v1/device/attest${q}`, {
        headers: { authorization: `Bearer ${TOKEN}`, ...headers },
      });
    let a = await attest({ "x-pocket-sd": "0" });
    expect(a.status).toBe(200);
    expect((await a.json()).sd_present).toBe(false);
    expect(sdOf()).toBe(0);

    a = await attest({});
    expect((await a.json()).sd_present).toBe(false);

    a = await attest({}, "?sd=1");
    expect((await a.json()).sd_present).toBe(true);
    expect(sdOf()).toBe(1);
  });
});
