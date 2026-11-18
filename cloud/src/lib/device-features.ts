/**
 * Reading progress, Find my Pocket, and Home screen order — shared validation.
 * Production mirror: neon-fn/scram-api.mjs (keep both in step).
 */

/** Home screen app keys the firmware knows, in the default order. */
export const HOME_KEYS = [
  "today",
  "notes",
  "clock",
  "weather",
  "music",
  "reading",
  "pass",
  "settings",
  "update",
] as const;
export type HomeKey = (typeof HOME_KEYS)[number];
export const HOME_ORDER_MAX = 16;

/** A ring queued from Companion is delivered once on the next attest, or dropped after this. */
export const RING_TTL_MS = 30 * 60 * 1000;

export type ReadingProgressInput = {
  book_id: string;
  page: number;
  pages: number;
  percent: number;
};

const isInt = (v: unknown): v is number => typeof v === "number" && Number.isInteger(v);

/** POST /v1/device/reading body → normalized record, or an error message. */
export function parseReadingProgress(
  body: unknown,
): { ok: true; value: ReadingProgressInput } | { ok: false; message: string } {
  const bad = { ok: false as const, message: "That reading update couldn't be read." };
  if (!body || typeof body !== "object" || Array.isArray(body)) return bad;
  const b = body as Record<string, unknown>;
  const bookId = typeof b.book_id === "string" ? b.book_id.trim() : "";
  if (!bookId || bookId.length > 128) return bad;
  if (!isInt(b.page) || !isInt(b.pages) || !isInt(b.percent)) return bad;
  if (b.page < 0 || b.pages < 0 || b.page > 1_000_000 || b.pages > 1_000_000) return bad;
  if (b.percent < 0 || b.percent > 100) return bad;
  return { ok: true, value: { book_id: bookId, page: b.page, pages: b.pages, percent: b.percent } };
}

/** PATCH home_order → validated keys, or an error message. Empty array = default order. */
export function parseHomeOrder(
  raw: unknown,
): { ok: true; value: HomeKey[] } | { ok: false; message: string } {
  if (!Array.isArray(raw)) return { ok: false, message: "Home screen order must be a list." };
  if (raw.length > HOME_ORDER_MAX) {
    return { ok: false, message: "Home screen order has too many apps." };
  }
  const seen = new Set<string>();
  for (const k of raw) {
    if (typeof k !== "string" || !(HOME_KEYS as readonly string[]).includes(k)) {
      return { ok: false, message: "Home screen order has an app Pocket doesn't know." };
    }
    if (seen.has(k)) return { ok: false, message: "Home screen order lists an app twice." };
    seen.add(k);
  }
  return { ok: true, value: raw as HomeKey[] };
}

/** Stored JSON → home order (drops anything unknown; [] when unset or unreadable). */
export function readHomeOrder(json: unknown): HomeKey[] {
  if (typeof json !== "string" || !json) return [];
  try {
    const parsed = parseHomeOrder(JSON.parse(json));
    return parsed.ok ? parsed.value : [];
  } catch {
    return [];
  }
}

/** Whether a ring queued at `queuedAt` should still chime at `now`. */
export function ringIsFresh(queuedAt: string | null | undefined, now: Date): boolean {
  if (!queuedAt) return false;
  const t = Date.parse(queuedAt);
  if (!Number.isFinite(t)) return false;
  return now.getTime() - t <= RING_TTL_MS;
}
