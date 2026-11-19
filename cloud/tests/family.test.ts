import { describe, expect, it, beforeEach } from "vitest";
import { openMemoryDatabase, closeDatabase, run, get } from "../src/db/index.js";
import { createApp } from "../src/app.js";
import { getOrCreateUser, createSession, upsertSubscriptionMirror } from "../src/lib/auth.js";
import { sha256Hex } from "../src/lib/crypto.js";

type App = ReturnType<typeof createApp>;

function entitle(userId: string) {
  upsertSubscriptionMirror(userId, {
    status: "trialing",
    trial_ends_at: new Date(Date.now() + 7 * 86400000).toISOString(),
  });
}

function linkDevice(userId: string, deviceId: string, token: string) {
  run(
    `INSERT INTO device_links (id, user_id, device_id, device_name, linked_at, last_seen_at, device_token_hash)
     VALUES (?, ?, ?, 'Pocket', ?, NULL, ?)`,
    [`link-${deviceId}`, userId, deviceId, new Date().toISOString(), sha256Hex(token)],
  );
}

async function call(app: App, method: string, path: string, headers: Record<string, string>, body?: unknown) {
  const res = await app.request(`http://localhost${path}`, {
    method,
    headers: { "content-type": "application/json", ...headers },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await res.text();
  return { status: res.status, body: (text ? JSON.parse(text) : null) as any };
}

const iso = (msAgo: number) => new Date(Date.now() - msAgo).toISOString();

type Person = { id: string; s: Record<string, string> };

function person(email: string): Person {
  const id = getOrCreateUser(email).id;
  entitle(id);
  return { id, s: { cookie: `pocket_session=${createSession(id).token}` } };
}

describe("Pocket Family", () => {
  let app: App;
  let owner: Person;
  let kid: Person;
  let outsider: Person;

  beforeEach(async () => {
    closeDatabase();
    await openMemoryDatabase();
    app = createApp();
    owner = person("dana@example.com");
    kid = person("sam@example.com");
    outsider = person("eve@example.com");
  });

  async function makeFamily(): Promise<string> {
    const r = await call(app, "POST", "/v1/family", owner.s, { name: "The Rivera house" });
    expect(r.status).toBe(201);
    return r.body.family.join_code;
  }

  it("creates a family, joins by code, lists members, leaves", async () => {
    expect((await call(app, "GET", "/v1/family", owner.s)).body).toEqual({ family: null });
    expect((await call(app, "POST", "/v1/family", owner.s, { name: "  " })).status).toBe(400);

    const created = await call(app, "POST", "/v1/family", owner.s, { name: "The Rivera house" });
    expect(created.status).toBe(201);
    const fam = created.body.family;
    expect(fam).toMatchObject({ name: "The Rivera house", role: "owner", max_members: 6 });
    expect(fam.join_code).toMatch(/^[A-HJ-NP-Z2-9]{8}$/);
    const expiresIn = Date.parse(fam.join_code_expires_at) - Date.now();
    expect(expiresIn).toBeGreaterThan(6.9 * 86400000);
    expect(expiresIn).toBeLessThanOrEqual(7 * 86400000);
    expect(fam.members).toEqual([
      expect.objectContaining({ user_id: owner.id, name: "dana", role: "owner", is_me: true }),
    ]);
    // One family per person.
    expect((await call(app, "POST", "/v1/family", owner.s, { name: "Second" })).status).toBe(409);

    // Lower-case + dashes are fine.
    const code = `${fam.join_code.slice(0, 4).toLowerCase()}-${fam.join_code.slice(4)}`;
    const joined = await call(app, "POST", "/v1/family/join", kid.s, { code });
    expect(joined.status).toBe(200);
    expect(joined.body.family.role).toBe("member");
    expect(joined.body.family.join_code).toBeNull(); // only the owner sees the invite code
    expect(joined.body.family.members.map((m: any) => m.name)).toEqual(["dana", "sam"]);

    expect((await call(app, "POST", "/v1/family/join", outsider.s, { code: "ZZZZZZZZ" })).status).toBe(404);

    // Members can leave; the owner deletes instead.
    expect((await call(app, "POST", "/v1/family/leave", owner.s)).status).toBe(400);
    expect((await call(app, "POST", "/v1/family/leave", kid.s)).body).toEqual({ ok: true });
    expect((await call(app, "GET", "/v1/family", kid.s)).body.family).toBeNull();
    const after = await call(app, "GET", "/v1/family", owner.s);
    expect(after.body.family.members).toHaveLength(1);
  });

  it("caps families at 6 and rejects expired codes", async () => {
    const code = await makeFamily();
    for (let i = 0; i < 5; i++) {
      const p = person(`member${i}@example.com`);
      expect((await call(app, "POST", "/v1/family/join", p.s, { code })).status).toBe(200);
    }
    const seventh = await call(app, "POST", "/v1/family/join", kid.s, { code });
    expect(seventh.status).toBe(409);
    expect(seventh.body.error.code).toBe("family_full");

    run("UPDATE families SET join_code_expires_at = ?", [iso(1000)]);
    const expired = await call(app, "POST", "/v1/family/join", outsider.s, { code });
    expect(expired.status).toBe(410);

    // A fresh code works again (but the family is still full).
    const fresh = await call(app, "POST", "/v1/family/code", owner.s);
    expect(fresh.body.family.join_code).not.toBe(code);
    expect((await call(app, "POST", "/v1/family/code", kid.s)).status).toBe(404);
  });

  it("owner removes a member; only the owner can", async () => {
    const code = await makeFamily();
    await call(app, "POST", "/v1/family/join", kid.s, { code });
    await call(app, "POST", "/v1/family/join", outsider.s, { code });
    expect((await call(app, "DELETE", `/v1/family/members/${outsider.id}`, kid.s)).status).toBe(403);
    expect((await call(app, "DELETE", `/v1/family/members/${owner.id}`, owner.s)).status).toBe(400);
    const r = await call(app, "DELETE", `/v1/family/members/${outsider.id}`, owner.s);
    expect(r.status).toBe(200);
    expect(r.body.family.members.map((m: any) => m.name)).toEqual(["dana", "sam"]);
    expect((await call(app, "GET", "/v1/family", outsider.s)).body.family).toBeNull();
  });

  describe("shared lists", () => {
    async function ownerList(title = "Groceries"): Promise<string> {
      const r = await call(app, "POST", "/v1/lists", owner.s, {
        title,
        items: [{ id: "i1", text: "Milk", checked: false, updated_at: iso(60_000) }],
        updated_at: iso(60_000),
        updated_by_device_id: "pwa",
      });
      return r.body.list.id;
    }

    it("is visible and editable to members, invisible to outsiders", async () => {
      const code = await makeFamily();
      await call(app, "POST", "/v1/family/join", kid.s, { code });
      const id = await ownerList();

      // Not shared yet: the member can't see it.
      expect((await call(app, "GET", "/v1/lists", kid.s)).body.lists).toEqual([]);
      expect((await call(app, "GET", `/v1/lists/${id}`, kid.s)).status).toBe(404);
      // Only the owner can share it.
      expect((await call(app, "POST", `/v1/lists/${id}/share`, kid.s, { shared: true })).status).toBe(404);
      // Outsider (no family) can't share their own list.
      const lone = await call(app, "POST", "/v1/lists", outsider.s, {
        title: "Mine",
        updated_at: iso(0),
        updated_by_device_id: "pwa",
      });
      expect(
        (await call(app, "POST", `/v1/lists/${lone.body.list.id}/share`, outsider.s, { shared: true })).status,
      ).toBe(400);

      const shared = await call(app, "POST", `/v1/lists/${id}/share`, owner.s, { shared: true });
      expect(shared.status).toBe(200);
      expect(shared.body.list).toMatchObject({ id, shared: true, is_owner: true, owner_name: "dana" });

      const seen = await call(app, "GET", "/v1/lists", kid.s);
      expect(seen.body.lists).toEqual([
        expect.objectContaining({ id, title: "Groceries", shared: true, is_owner: false, owner_name: "dana" }),
      ]);
      expect((await call(app, "GET", "/v1/lists", owner.s)).body.lists[0]).toMatchObject({ shared: true });

      // Member edits items.
      const edited = await call(app, "PATCH", `/v1/lists/${id}`, kid.s, {
        items: [
          { id: "i1", text: "Milk", checked: true, updated_at: iso(0) },
          { id: "i2", text: "Eggs", checked: false, updated_at: iso(0) },
        ],
        updated_at: iso(0),
        updated_by_device_id: "pwa",
      });
      expect(edited.status).toBe(200);
      const ownerView = await call(app, "GET", `/v1/lists/${id}`, owner.s);
      expect(ownerView.body.list.items.map((i: any) => [i.text, i.checked])).toEqual([
        ["Milk", true],
        ["Eggs", false],
      ]);
      expect(get<{ user_id: string }>("SELECT user_id FROM cloud_lists WHERE id = ?", [id])!.user_id).toBe(
        owner.id,
      );

      // Member can't delete or unshare; outsider can't touch it at all.
      expect((await call(app, "DELETE", `/v1/lists/${id}`, kid.s)).status).toBe(403);
      expect((await call(app, "GET", "/v1/lists", outsider.s)).body.lists.map((l: any) => l.id)).not.toContain(id);
      expect((await call(app, "GET", `/v1/lists/${id}`, outsider.s)).status).toBe(404);
      expect(
        (await call(app, "PATCH", `/v1/lists/${id}`, outsider.s, { title: "pwned", updated_at: iso(0), updated_by_device_id: "x" }))
          .status,
      ).toBe(404);
      expect((await call(app, "DELETE", `/v1/lists/${id}`, outsider.s)).status).toBe(404);

      // Leaving the family ends access; unsharing does too.
      await call(app, "POST", `/v1/lists/${id}/share`, owner.s, { shared: false });
      expect((await call(app, "GET", "/v1/lists", kid.s)).body.lists).toEqual([]);
    });

    it("a member's own shared lists are unshared when they leave", async () => {
      const code = await makeFamily();
      await call(app, "POST", "/v1/family/join", kid.s, { code });
      const mine = await call(app, "POST", "/v1/lists", kid.s, {
        title: "Sam's wishlist",
        updated_at: iso(0),
        updated_by_device_id: "pwa",
      });
      await call(app, "POST", `/v1/lists/${mine.body.list.id}/share`, kid.s, { shared: true });
      expect((await call(app, "GET", "/v1/lists", owner.s)).body.lists).toHaveLength(1);
      await call(app, "POST", "/v1/family/leave", kid.s);
      expect((await call(app, "GET", "/v1/lists", owner.s)).body.lists).toHaveLength(0);
      expect(get<{ family_id: string | null }>("SELECT family_id FROM cloud_lists WHERE id = ?", [mine.body.list.id])!.family_id).toBeNull();
    });

    it("device sync returns shared lists and accepts member pushes", async () => {
      const code = await makeFamily();
      await call(app, "POST", "/v1/family/join", kid.s, { code });
      const id = await ownerList();
      await call(app, "POST", `/v1/lists/${id}/share`, owner.s, { shared: true });
      linkDevice(kid.id, "dev-kid", "tok-kid");
      linkDevice(outsider.id, "dev-eve", "tok-eve");
      const sync = (token: string, body: unknown) =>
        call(app, "POST", "/v1/device/sync", { authorization: `Bearer ${token}` }, body);

      let r = await sync("tok-kid", {});
      expect(r.status).toBe(200);
      expect(r.body.lists).toEqual([
        expect.objectContaining({ id, title: "Groceries", shared: true, owner: "dana" }),
      ]);

      // Member pushes a newer copy: accepted (LWW), stays the owner's list.
      r = await sync("tok-kid", {
        lists: [
          {
            id,
            title: "Groceries",
            items: [{ id: "i1", text: "Oat milk", checked: false, updated_at: iso(0) }],
            created_at: iso(60_000),
            updated_at: iso(0),
          },
        ],
      });
      expect(r.body.skipped.lists).toEqual([]);
      expect(r.body.lists[0].items[0].text).toBe("Oat milk");
      const row = get<{ user_id: string; updated_by_device_id: string }>(
        "SELECT user_id, updated_by_device_id FROM cloud_lists WHERE id = ?",
        [id],
      )!;
      expect(row).toEqual({ user_id: owner.id, updated_by_device_id: "dev-kid" });

      // An older push loses (LWW unchanged).
      r = await sync("tok-kid", { lists: [{ id, title: "Stale", items: [], updated_at: iso(3600_000) }] });
      expect(r.body.lists[0].title).toBe("Groceries");

      // Members can't delete a shared list from their Pocket.
      r = await sync("tok-kid", { lists: [{ id, title: "Groceries", items: [], updated_at: iso(-1000), deleted_at: iso(-1000) }] });
      expect(r.body.skipped.lists).toEqual([{ id, reason: "not_owner" }]);
      expect(get<{ deleted_at: string | null }>("SELECT deleted_at FROM cloud_lists WHERE id = ?", [id])!.deleted_at).toBeNull();

      // Outsider never sees it and can't write it.
      r = await sync("tok-eve", {
        lists: [{ id, title: "pwned", items: [], updated_at: iso(-2000) }],
      });
      expect(r.body.lists.map((l: any) => l.id)).not.toContain(id);
      expect(r.body.skipped.lists).toEqual([{ id, reason: "unavailable" }]);
      expect(get<{ title: string }>("SELECT title FROM cloud_lists WHERE id = ?", [id])!.title).toBe("Groceries");

      // The owner's own sync marks it shared too; unshared lists say shared:false.
      linkDevice(owner.id, "dev-dana", "tok-dana");
      await ownerList("Private");
      r = await sync("tok-dana", {});
      const byTitle = Object.fromEntries(r.body.lists.map((l: any) => [l.title, l]));
      expect(byTitle.Groceries).toMatchObject({ shared: true, owner: "dana" });
      expect(byTitle.Private.shared).toBe(false);
    });
  });
});
