import { Hono } from "hono";
import type { AppEnv } from "../middleware/auth.js";
import { requireDeviceToken, requireSession } from "../middleware/auth.js";
import { all, get, run } from "../db/index.js";
import { newId } from "../lib/crypto.js";
import { AppError, Errors } from "../lib/errors.js";
import {
  CALENDAR_MAX_FEEDS,
  feedHost,
  normalizeFeedLabel,
  normalizeFeedUrl,
  upcomingEvents,
} from "../lib/calendar.js";

/**
 * Calendar on Today. Production mirror: neon-fn/scram-api.mjs.
 *   GET    /v1/calendar/feeds              (session) → { feeds: [{id,label,host,created_at}], max_feeds }
 *   POST   /v1/calendar/feeds {label,url}  (session) → 201 { feed }   (400 bad link, 409 at 5 feeds)
 *   DELETE /v1/calendar/feeds/:id          (session) → { ok }
 *   GET    /v1/device/calendar             (device)  → { events: [...], fetched_at }
 */
type FeedRow = { id: string; user_id: string; label: string; url: string; created_at: string };

function serializeFeed(f: FeedRow) {
  return { id: f.id, label: f.label, host: feedHost(f.url), created_at: f.created_at };
}

export const calendarRoutes = new Hono<AppEnv>();

calendarRoutes.use("*", requireSession);

calendarRoutes.get("/feeds", (c) => {
  const user = c.get("user")!;
  const rows = all<FeedRow>("SELECT * FROM calendar_feeds WHERE user_id = ? ORDER BY created_at ASC", [
    user.id,
  ]);
  return c.json({ feeds: rows.map(serializeFeed), max_feeds: CALENDAR_MAX_FEEDS });
});

calendarRoutes.post("/feeds", async (c) => {
  const user = c.get("user")!;
  const body = await c.req.json().catch(() => ({}));
  const url = normalizeFeedUrl(body?.url);
  if (!url.ok) throw Errors.badRequest(url.message);
  const n = get<{ n: number }>("SELECT COUNT(*) AS n FROM calendar_feeds WHERE user_id = ?", [user.id]);
  if (Number(n?.n ?? 0) >= CALENDAR_MAX_FEEDS) {
    throw new AppError(409, "calendar_limit", `You can add up to ${CALENDAR_MAX_FEEDS} calendars.`);
  }
  if (get("SELECT id FROM calendar_feeds WHERE user_id = ? AND url = ?", [user.id, url.value])) {
    throw Errors.conflict("That calendar is already added.");
  }
  const row: FeedRow = {
    id: newId(),
    user_id: user.id,
    label: normalizeFeedLabel(body?.label),
    url: url.value,
    created_at: new Date().toISOString(),
  };
  run("INSERT INTO calendar_feeds (id, user_id, label, url, created_at) VALUES (?, ?, ?, ?, ?)", [
    row.id,
    row.user_id,
    row.label,
    row.url,
    row.created_at,
  ]);
  return c.json({ feed: serializeFeed(row) }, 201);
});

calendarRoutes.delete("/feeds/:id", (c) => {
  const user = c.get("user")!;
  const row = get("SELECT id FROM calendar_feeds WHERE id = ? AND user_id = ?", [
    c.req.param("id"),
    user.id,
  ]);
  if (!row) throw Errors.notFound("That calendar");
  run("DELETE FROM calendar_feeds WHERE id = ?", [c.req.param("id")]);
  return c.json({ ok: true });
});

export const deviceCalendarRoutes = new Hono<AppEnv>();

deviceCalendarRoutes.get("/calendar", requireDeviceToken, async (c) => {
  const user = c.get("user")!;
  const feeds = all<FeedRow>("SELECT * FROM calendar_feeds WHERE user_id = ? ORDER BY created_at ASC", [
    user.id,
  ]);
  const now = new Date();
  const events = await upcomingEvents(feeds, now);
  return c.json({ events, fetched_at: now.toISOString() });
});
