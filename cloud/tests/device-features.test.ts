import { describe, expect, it, beforeEach, vi, afterEach } from "vitest";
import { openMemoryDatabase, closeDatabase, run, get } from "../src/db/index.js";
import { createApp } from "../src/app.js";
import { getOrCreateUser, createSession, upsertSubscriptionMirror } from "../src/lib/auth.js";
import { sha256Hex } from "../src/lib/crypto.js";
import { parseHomeOrder, parseReadingProgress } from "../src/lib/device-features.js";

type App = ReturnType<typeof createApp>;

const TOKEN = "tok-device-a";

function linkDevice(userId: string, deviceId: string, token: string) {
  run(
    `INSERT INTO device_links (id, user_id, device_id, device_name, linked_at, last_seen_at, device_token_hash)
     VALUES (?, ?, ?, 'Pocket', ?, NULL, ?)`,
    [`link-${deviceId}`, userId, deviceId, new Date().toISOString(), sha256Hex(token)],
  );
}

function addBook(userId: string, id: string, title: string) {
  run(
    `INSERT INTO books (id, user_id, title, author, format, filename, size_bytes, text_b64, created_at)
     VALUES (?, ?, ?, '', 'txt', ?, 10, '', ?)`,
    [id, userId, title, `${id}.txt`, new Date().toISOString()],
  );
}

async function call(app: App, method: string, path: string, headers: Record<string, string>, body?: unknown) {
  const res = await app.request(`http://localhost${path}`, {
    method,
    headers: { "content-type": "application/json", ...headers },
    body: body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body),
  });
  return { status: res.status, body: (await res.json()) as any };
}

describe("device features", () => {
  let app: App;
  let userId: string;
  let cookie: Record<string, string>;
  const device = { authorization: `Bearer ${TOKEN}` };

  beforeEach(async () => {
    closeDatabase();
    await openMemoryDatabase();
    app = createApp();
    userId = getOrCreateUser("features@example.com").id;
    linkDevice(userId, "dev-a", TOKEN);
    cookie = { cookie: `pocket_session=${createSession(userId).token}` };
    upsertSubscriptionMirror(userId, {
      status: "trialing",
      trial_ends_at: new Date(Date.now() + 7 * 86400000).toISOString(),
    });
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  describe("reading progress", () => {
    it("upserts progress and shows it in /v1/books", async () => {
      addBook(userId, "b1", "Moby Dick");
      addBook(userId, "b2", "Walden");

      let res = await call(app, "POST", "/v1/device/reading?device_id=dev-a", device, {
        book_id: "b1",
        page: 10,
        pages: 286,
        percent: 3,
      });
      expect(res).toEqual({ status: 200, body: { ok: true } });
      res = await call(app, "POST", "/v1/device/reading?device_id=dev-a", device, {
        book_id: "b1",
        page: 120,
        pages: 286,
        percent: 42,
      });
      expect(res.status).toBe(200);

      const count = get<{ n: number }>("SELECT COUNT(*) AS n FROM reading_progress");
      expect(count?.n).toBe(1);
      const stored = get<{ device_id: string }>("SELECT device_id FROM reading_progress");
      expect(stored?.device_id).toBe("dev-a");

      const list = await call(app, "GET", "/v1/books", cookie);
      expect(list.status).toBe(200);
      const byId = Object.fromEntries(list.body.books.map((b: any) => [b.id, b]));
      expect(byId.b1.progress).toMatchObject({ percent: 42, page: 120, pages: 286 });
      expect(typeof byId.b1.progress.updated_at).toBe("string");
      expect(byId.b2.progress).toBeNull();
    });

    it("stores progress for unknown book ids without failing", async () => {
      const res = await call(app, "POST", "/v1/device/reading", device, {
        book_id: "not-in-library",
        page: 1,
        pages: 2,
        percent: 50,
      });
      expect(res.status).toBe(200);
      const list = await call(app, "GET", "/v1/books", cookie);
      expect(list.body.books).toEqual([]);
    });

    it("rejects invalid bodies with 400", async () => {
      for (const body of [
        "not json",
        [],
        { page: 1, pages: 2, percent: 3 },
        { book_id: "", page: 1, pages: 2, percent: 3 },
        { book_id: "b1", page: "1", pages: 2, percent: 3 },
        { book_id: "b1", page: 1, pages: 2, percent: 101 },
        { book_id: "b1", page: -1, pages: 2, percent: 3 },
        { book_id: "b1", page: 1.5, pages: 2, percent: 3 },
      ]) {
        const res = await call(app, "POST", "/v1/device/reading", device, body);
        expect(res.status, JSON.stringify(body)).toBe(400);
      }
    });

    it("requires a device token", async () => {
      const res = await call(app, "POST", "/v1/device/reading", { authorization: "Bearer nope" }, {
        book_id: "b1",
        page: 1,
        pages: 2,
        percent: 3,
      });
      expect(res.status).toBe(401);
    });
  });

  describe("find my Pocket", () => {
    it("delivers a ring once on attest", async () => {
      let attest = await call(app, "GET", "/v1/device/attest", device);
      expect(attest.body.ring).toBe(false);

      const ring = await call(app, "POST", "/v1/devices/dev-a/ring", cookie);
      expect(ring.status).toBe(200);
      expect(ring.body.ok).toBe(true);
      expect(Number.isFinite(Date.parse(ring.body.queued_at))).toBe(true);

      attest = await call(app, "GET", "/v1/device/attest", device);
      expect(attest.body.ring).toBe(true);
      attest = await call(app, "GET", "/v1/device/attest", device);
      expect(attest.body.ring).toBe(false);
    });

    it("expires rings older than 30 minutes", async () => {
      vi.useFakeTimers({ toFake: ["Date"] });
      vi.setSystemTime(new Date("2026-11-18T10:00:00Z"));
      await call(app, "POST", "/v1/devices/link-dev-a/ring", cookie);
      vi.setSystemTime(new Date("2026-11-18T10:31:00Z"));
      let attest = await call(app, "GET", "/v1/device/attest", device);
      expect(attest.body.ring).toBe(false);
      const row = get<{ ring_requested_at: string | null }>(
        "SELECT ring_requested_at FROM device_links WHERE id = 'link-dev-a'",
      );
      expect(row?.ring_requested_at).toBeNull();

      await call(app, "POST", "/v1/devices/dev-a/ring", cookie);
      vi.setSystemTime(new Date("2026-11-18T10:59:00Z"));
      attest = await call(app, "GET", "/v1/device/attest", device);
      expect(attest.body.ring).toBe(true);
    });

    it("only rings devices you own", async () => {
      const other = getOrCreateUser("other@example.com").id;
      const otherCookie = { cookie: `pocket_session=${createSession(other).token}` };
      const res = await call(app, "POST", "/v1/devices/dev-a/ring", otherCookie);
      expect(res.status).toBe(404);
      const anon = await call(app, "POST", "/v1/devices/dev-a/ring", {});
      expect(anon.status).toBe(401);
    });
  });

  describe("home screen order", () => {
    it("defaults to an empty order", async () => {
      const attest = await call(app, "GET", "/v1/device/attest", device);
      expect(attest.body.home_order).toEqual([]);
      const dev = await call(app, "GET", "/v1/devices/dev-a", cookie);
      expect(dev.body.home_order).toEqual([]);
    });

    it("saves a valid order and delivers it on attest", async () => {
      const order = ["reading", "today", "notes", "clock", "settings", "update"];
      const res = await call(app, "PATCH", "/v1/devices/dev-a", cookie, { home_order: order });
      expect(res.status).toBe(200);
      expect(res.body.home_order).toEqual(order);
      expect(res.body.device_name).toBe("Pocket");

      const attest = await call(app, "GET", "/v1/device/attest", device);
      expect(attest.body.home_order).toEqual(order);
      const one = await call(app, "GET", "/v1/devices/link-dev-a", cookie);
      expect(one.body.home_order).toEqual(order);
      const list = await call(app, "GET", "/v1/devices", cookie);
      expect(list.body.devices[0].home_order).toEqual(order);
    });

    it("rejects unknown keys, duplicates and non-lists", async () => {
      for (const home_order of [
        ["today", "games"],
        ["today", "today"],
        "today",
        [1, 2],
        Array.from({ length: 17 }, () => "today"),
      ]) {
        const res = await call(app, "PATCH", "/v1/devices/dev-a", cookie, { home_order });
        expect(res.status, JSON.stringify(home_order)).toBe(400);
      }
      const attest = await call(app, "GET", "/v1/device/attest", device);
      expect(attest.body.home_order).toEqual([]);
    });

    it("still renames devices", async () => {
      const res = await call(app, "PATCH", "/v1/devices/dev-a", cookie, { device_name: "Desk" });
      expect(res.status).toBe(200);
      expect(res.body.device_name).toBe("Desk");
      const empty = await call(app, "PATCH", "/v1/devices/dev-a", cookie, {});
      expect(empty.status).toBe(400);
    });
  });

  it("validators match the contract", () => {
    expect(parseHomeOrder([]).ok).toBe(true);
    expect(
      parseHomeOrder(["today", "notes", "clock", "weather", "music", "reading", "pass", "settings", "update"]).ok,
    ).toBe(true);
    expect(parseReadingProgress({ book_id: "x", page: 0, pages: 0, percent: 0 }).ok).toBe(true);
    expect(parseReadingProgress({ book_id: "x", page: 0, pages: 0, percent: 100 }).ok).toBe(true);
  });
});
