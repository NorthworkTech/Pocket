import { describe, expect, it, beforeEach, afterEach, vi } from "vitest";
import { openMemoryDatabase, closeDatabase, run } from "../src/db/index.js";
import { createApp } from "../src/app.js";
import { getOrCreateUser, createSession, upsertSubscriptionMirror } from "../src/lib/auth.js";
import { sha256Hex } from "../src/lib/crypto.js";
import { clearCalendarCache, normalizeFeedUrl } from "../src/lib/calendar.js";

type App = ReturnType<typeof createApp>;

function linkDevice(userId: string, deviceId: string, token: string, name = "Pocket") {
  run(
    `INSERT INTO device_links (id, user_id, device_id, device_name, linked_at, last_seen_at, device_token_hash)
     VALUES (?, ?, ?, ?, ?, NULL, ?)`,
    [`link-${deviceId}`, userId, deviceId, name, new Date().toISOString(), sha256Hex(token)],
  );
}

async function call(app: App, method: string, path: string, headers: Record<string, string>, body?: unknown) {
  const res = await app.request(`http://localhost${path}`, {
    method,
    headers: { "content-type": "application/json", ...headers },
    body: body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body),
  });
  const text = await res.text();
  return { status: res.status, body: (text ? JSON.parse(text) : null) as any };
}

const ics = (...events: string[]) =>
  ["BEGIN:VCALENDAR", "VERSION:2.0", ...events, "END:VCALENDAR"].join("\r\n");
const stamp = (d: Date) => d.toISOString().replace(/[-:]/g, "").replace(/\.\d{3}/, "");

describe("calendar + device backup", () => {
  let app: App;
  let userId: string;
  let session: Record<string, string>;
  const dev = { authorization: "Bearer tok-a" };
  const devB = { authorization: "Bearer tok-b" };

  beforeEach(async () => {
    closeDatabase();
    await openMemoryDatabase();
    app = createApp();
    userId = getOrCreateUser("cal@example.com").id;
    upsertSubscriptionMirror(userId, {
      status: "trialing",
      trial_ends_at: new Date(Date.now() + 7 * 86400000).toISOString(),
    });
    session = { cookie: `pocket_session=${createSession(userId).token}` };
    linkDevice(userId, "dev-a", "tok-a", "Kitchen");
    linkDevice(userId, "dev-b", "tok-b", "New Pocket");
    clearCalendarCache();
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  describe("calendar feeds", () => {
    it("validates links (webcal → https, https only)", () => {
      expect(normalizeFeedUrl("webcal://p01-caldav.icloud.com/published/2/abc")).toEqual({
        ok: true,
        value: "https://p01-caldav.icloud.com/published/2/abc",
      });
      for (const bad of ["http://calendar.google.com/x.ics", "ftp://x", "nope", "https://localhost/x", "https://10.0.0.1/x", ""]) {
        expect(normalizeFeedUrl(bad).ok).toBe(false);
      }
    });

    it("adds, lists (without the secret path), caps at 5, removes", async () => {
      let r = await call(app, "POST", "/v1/calendar/feeds", session, {
        label: "Family",
        url: "webcal://calendar.google.com/calendar/ical/secret/basic.ics",
      });
      expect(r.status).toBe(201);
      expect(r.body.feed).toMatchObject({ label: "Family", host: "calendar.google.com" });
      expect(JSON.stringify(r.body)).not.toContain("secret");
      expect((await call(app, "POST", "/v1/calendar/feeds", session, { label: "x", url: "http://a.com/x" })).status).toBe(400);
      for (let i = 2; i <= 5; i++) {
        r = await call(app, "POST", "/v1/calendar/feeds", session, { label: `Cal ${i}`, url: `https://ex${i}.com/c.ics` });
        expect(r.status).toBe(201);
      }
      r = await call(app, "POST", "/v1/calendar/feeds", session, { label: "Six", url: "https://ex6.com/c.ics" });
      expect(r.status).toBe(409);
      r = await call(app, "GET", "/v1/calendar/feeds", session);
      expect(r.body.feeds).toHaveLength(5);
      expect(r.body.max_feeds).toBe(5);
      const id = r.body.feeds[0].id;
      expect((await call(app, "DELETE", `/v1/calendar/feeds/${id}`, session)).body).toEqual({ ok: true });
      expect((await call(app, "GET", "/v1/calendar/feeds", session)).body.feeds).toHaveLength(4);
      expect((await call(app, "GET", "/v1/calendar/feeds", {})).status).toBe(401);
    });

    it("GET /v1/device/calendar merges feeds, skips failing ones, caches 10 minutes", async () => {
      await call(app, "POST", "/v1/calendar/feeds", session, { label: "Home", url: "https://cal.example.com/home.ics" });
      await call(app, "POST", "/v1/calendar/feeds", session, { label: "Work", url: "https://cal.example.com/work.ics" });
      await call(app, "POST", "/v1/calendar/feeds", session, { label: "Broken", url: "https://down.example.com/x.ics" });
      const now = Date.now();
      const inH = (h: number) => stamp(new Date(now + h * 3600000));
      const today = new Date(now).toISOString().slice(0, 10).replace(/-/g, "");
      const bodies: Record<string, string> = {
        "https://cal.example.com/home.ics": ics(
          "BEGIN:VEVENT", "UID:1", `DTSTART:${inH(5)}`, `DTEND:${inH(6)}`, "SUMMARY:Dentist", "LOCATION:High St", "END:VEVENT",
          "BEGIN:VEVENT", "UID:2", `DTSTART;VALUE=DATE:${today}`, "SUMMARY:Bin day", "END:VEVENT",
          "BEGIN:VEVENT", "UID:3", `DTSTART:${inH(24 * 9)}`, "SUMMARY:Too far", "END:VEVENT",
        ),
        "https://cal.example.com/work.ics": ics(
          "BEGIN:VEVENT", "UID:w", `DTSTART:${inH(2)}`, `DTEND:${inH(3)}`, "RRULE:FREQ=DAILY;COUNT=3", "SUMMARY:Standup", "END:VEVENT",
        ),
      };
      const fetchMock = vi.fn(async (url: string | URL) => {
        const body = bodies[String(url)];
        if (!body) return new Response("nope", { status: 500 });
        return new Response(body, { status: 200, headers: { "content-type": "text/calendar" } });
      });
      vi.stubGlobal("fetch", fetchMock);

      let r = await call(app, "GET", "/v1/device/calendar?device_id=dev-a", dev);
      expect(r.status).toBe(200);
      expect(Object.keys(r.body).sort()).toEqual(["events", "fetched_at"]);
      expect(r.body.fetched_at).toMatch(/^\d{4}-\d\d-\d\dT.*Z$/);
      const titles = r.body.events.map((e: any) => e.title);
      expect(titles).toEqual(["Bin day", "Standup", "Dentist", "Standup", "Standup"]);
      expect(r.body.events[0]).toEqual({
        title: "Bin day",
        start: `${new Date(now).toISOString().slice(0, 10)}T00:00:00.000Z`,
        end: expect.any(String),
        all_day: true,
        location: "",
        calendar: "Home",
      });
      expect(r.body.events[2]).toMatchObject({ title: "Dentist", location: "High St", all_day: false, calendar: "Home" });
      expect(r.body.events[1].calendar).toBe("Work");
      expect(fetchMock).toHaveBeenCalledTimes(3);

      // Cached: no refetch of the good feeds within 10 minutes.
      await call(app, "GET", "/v1/device/calendar?device_id=dev-a", dev);
      expect(fetchMock).toHaveBeenCalledTimes(4); // only the broken one again

      expect((await call(app, "GET", "/v1/device/calendar", {})).status).toBe(401);
    });

    it("returns an empty list with no feeds", async () => {
      const r = await call(app, "GET", "/v1/device/calendar", dev);
      expect(r.body.events).toEqual([]);
    });
  });

  describe("device backup", () => {
    const payload = {
      settings: { quiet_hours: { start: "22:00", end: "07:00" }, volume: 60 },
      alarms: [{ time: "07:00", days: [1, 2, 3, 4, 5] }, { time: "09:00", days: [6] }],
      reading: { b1: { page: 12, pages: 300 } },
    };

    it("stores, returns the latest across Pockets, keeps 5", async () => {
      expect((await call(app, "GET", "/v1/device/backup/latest", devB)).status).toBe(404);
      vi.useFakeTimers({ toFake: ["Date"] });
      vi.setSystemTime(new Date("2026-11-19T10:00:00Z"));
      let r = await call(app, "POST", "/v1/device/backup?device_id=dev-a", dev, payload);
      expect(r.status).toBe(200);
      expect(r.body).toEqual({ ok: true, backup_id: expect.any(String), created_at: "2026-11-19T10:00:00.000Z" });
      const firstId = r.body.backup_id;

      // A brand-new Pocket on the same account restores from it.
      r = await call(app, "GET", "/v1/device/backup/latest?device_id=dev-b", devB);
      expect(r.body).toEqual({
        backup: {
          id: firstId,
          created_at: "2026-11-19T10:00:00.000Z",
          device_id: "dev-a",
          device_name: "Kitchen",
          ...payload,
        },
      });

      for (let i = 1; i <= 5; i++) {
        vi.setSystemTime(new Date(Date.parse("2026-11-19T10:00:00Z") + i * 60000));
        await call(app, "POST", "/v1/device/backup", dev, { ...payload, settings: { n: i } });
      }
      r = await call(app, "GET", "/v1/backups/devices", session);
      expect(r.body.backups).toHaveLength(5);
      expect(r.body.backups.map((b: any) => b.id)).not.toContain(firstId);
      expect(r.body.backups[0]).toMatchObject({
        device_name: "Kitchen",
        settings_count: 1,
        alarm_count: 2,
        reading_count: 1,
        created_at: "2026-11-19T10:05:00.000Z",
      });
      r = await call(app, "GET", "/v1/device/backup/latest", devB);
      expect(r.body.backup.settings).toEqual({ n: 5 });
    });

    it("rejects bad or oversized bodies", async () => {
      expect((await call(app, "POST", "/v1/device/backup", dev, { settings: [], alarms: [] })).status).toBe(400);
      expect((await call(app, "POST", "/v1/device/backup", dev, "not json")).status).toBe(400);
      expect((await call(app, "POST", "/v1/device/backup", dev, {})).status).toBe(400);
      const big = { settings: { blob: "x".repeat(65 * 1024) }, alarms: [], reading: {} };
      expect((await call(app, "POST", "/v1/device/backup", dev, big)).status).toBe(413);
      expect((await call(app, "POST", "/v1/device/backup", {}, payload)).status).toBe(401);
    });

    it("Back up now: request → attest backup_now once, expires after 24 h", async () => {
      let r = await call(app, "GET", "/v1/device/attest", dev);
      expect(r.body.backup_now).toBe(false);
      r = await call(app, "POST", "/v1/devices/dev-a/backup-request", session);
      expect(r.status).toBe(200);
      expect(r.body).toEqual({ ok: true, queued_at: expect.any(String) });
      r = await call(app, "GET", "/v1/backups/devices", session);
      expect(r.body.pending_requests).toEqual([{ device_id: "dev-a", requested_at: expect.any(String) }]);
      // The other Pocket isn't asked.
      expect((await call(app, "GET", "/v1/device/attest", devB)).body.backup_now).toBe(false);
      expect((await call(app, "GET", "/v1/device/attest", dev)).body.backup_now).toBe(true);
      expect((await call(app, "GET", "/v1/device/attest", dev)).body.backup_now).toBe(false);

      await call(app, "POST", "/v1/devices/link-dev-a/backup-request", session);
      run("UPDATE device_links SET backup_requested_at = ? WHERE id = 'link-dev-a'", [
        new Date(Date.now() - 25 * 3600000).toISOString(),
      ]);
      expect((await call(app, "GET", "/v1/device/attest", dev)).body.backup_now).toBe(false);

      // Must own the device.
      const other = getOrCreateUser("other@example.com").id;
      const otherSession = { cookie: `pocket_session=${createSession(other).token}` };
      expect((await call(app, "POST", "/v1/devices/dev-a/backup-request", otherSession)).status).toBe(404);
    });
  });
});
