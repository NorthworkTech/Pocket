/**
 * Calendar feeds for Today: validation of pasted iCal links, and fetching/caching each feed.
 * Production mirror: neon-fn/scram-api.mjs (keep both in step).
 */
import { expandEvents, mergeCalendarEvents, parseIcs, type CalendarEvent, type IcsEvent } from "./ics.js";

export const CALENDAR_MAX_FEEDS = 5;
export const CALENDAR_LABEL_MAX = 40;
export const CALENDAR_URL_MAX = 2048;
export const CALENDAR_WINDOW_DAYS = 7;
export const CALENDAR_MAX_EVENTS = 30;
export const CALENDAR_CACHE_MS = 10 * 60 * 1000;
export const CALENDAR_FETCH_TIMEOUT_MS = 8000;
export const CALENDAR_MAX_BYTES = 5 * 1024 * 1024;

/** webcal:// → https://; only public https URLs are accepted. */
export function normalizeFeedUrl(
  raw: unknown,
): { ok: true; value: string } | { ok: false; message: string } {
  const bad = { ok: false as const, message: "Paste the calendar's iCal link (it starts with https:// or webcal://)." };
  let s = typeof raw === "string" ? raw.trim() : "";
  if (!s || s.length > CALENDAR_URL_MAX) return bad;
  if (/^webcals?:\/\//i.test(s)) s = s.replace(/^webcals?:\/\//i, "https://");
  let u: URL;
  try {
    u = new URL(s);
  } catch {
    return bad;
  }
  if (u.protocol !== "https:" || u.username || u.password) return bad;
  const host = u.hostname.toLowerCase().replace(/^\[|\]$/g, "");
  if (
    host === "localhost" ||
    host.endsWith(".localhost") ||
    host.endsWith(".local") ||
    host.endsWith(".internal") ||
    /^\d+\.\d+\.\d+\.\d+$/.test(host) ||
    host.includes(":")
  ) {
    return { ok: false, message: "That calendar link isn't a public web address." };
  }
  return { ok: true, value: u.toString() };
}

export function normalizeFeedLabel(raw: unknown): string {
  const s = typeof raw === "string" ? raw.replace(/\s+/g, " ").trim() : "";
  return Array.from(s || "Calendar").slice(0, CALENDAR_LABEL_MAX).join("");
}

/** For Companion: show where a feed points without echoing its secret path. */
export function feedHost(url: string): string {
  try {
    return new URL(url).hostname;
  } catch {
    return "";
  }
}

const cache = new Map<string, { at: number; events: IcsEvent[] }>();

export function clearCalendarCache(): void {
  cache.clear();
}

/** Fetch + parse one feed (cached ~10 minutes). Throws on network/HTTP failure. */
export async function loadFeed(url: string, now = Date.now()): Promise<IcsEvent[]> {
  const hit = cache.get(url);
  if (hit && now - hit.at < CALENDAR_CACHE_MS) return hit.events;
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), CALENDAR_FETCH_TIMEOUT_MS);
  try {
    const res = await fetch(url, {
      signal: ctrl.signal,
      redirect: "follow",
      headers: { accept: "text/calendar, */*;q=0.5", "user-agent": "PocketCloud-Calendar/1.0" },
    });
    if (!res.ok) throw new Error(`calendar HTTP ${res.status}`);
    const text = await res.text();
    if (text.length > CALENDAR_MAX_BYTES) throw new Error("calendar too large");
    if (!/BEGIN:VCALENDAR/i.test(text)) throw new Error("not an iCalendar feed");
    const events = parseIcs(text);
    cache.set(url, { at: now, events });
    return events;
  } finally {
    clearTimeout(timer);
  }
}

/** Today's window across a user's feeds. A failing feed is skipped, never fatal. */
export async function upcomingEvents(
  feeds: { label: string; url: string }[],
  now = new Date(),
): Promise<CalendarEvent[]> {
  const to = new Date(now.getTime() + CALENDAR_WINDOW_DAYS * 86400000);
  const perFeed = await Promise.all(
    feeds.map(async (f) => {
      try {
        return expandEvents(await loadFeed(f.url, now.getTime()), now, to, f.label);
      } catch {
        return [];
      }
    }),
  );
  return mergeCalendarEvents(perFeed, CALENDAR_MAX_EVENTS);
}
