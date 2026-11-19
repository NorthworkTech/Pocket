/**
 * Device ↔ cloud notes/lists sync (POST /v1/device/sync) — input validation.
 * Production mirror lives in neon-fn/scram-api.mjs (syncNormalizeNote / syncNormalizeList);
 * keep limits and rules in step.
 */
import { normalizeUpdatedAt, type ListItem } from "./lww.js";

export const SYNC_MAX_RECORDS = 1000; // per kind (notes / lists) per request
export const SYNC_MAX_LIST_ITEMS = 500;
export const SYNC_MAX_TITLE_CHARS = 300;
export const SYNC_MAX_NOTE_BODY_BYTES = 200 * 1024;
export const SYNC_MAX_ID_CHARS = 200;
export const SYNC_TOMBSTONE_DAYS = 30;

export type SyncSkip = {
  id: string | null;
  reason: "invalid" | "too_large" | "unavailable" | "not_owner";
};

export type SyncEnvelope = {
  id: string;
  created_at: string;
  updated_at: string;
  deleted_at: string | null;
  updated_by_device_id: string;
};

export type SyncNote = SyncEnvelope & { title: string; body: string };
export type SyncList = SyncEnvelope & { title: string; items: ListItem[] };

type Raw = Record<string, unknown>;

function cleanText(v: unknown): string {
  return typeof v === "string" ? v.replace(/\u0000/g, "") : "";
}

function syncId(v: unknown): string | null {
  if (typeof v !== "string") return null;
  const s = v.trim();
  if (!s || s.length > SYNC_MAX_ID_CHARS || s.includes("\u0000")) return null;
  return s;
}

function rawId(raw: unknown): string | null {
  const id = raw && typeof raw === "object" ? (raw as Raw).id : undefined;
  return typeof id === "string" ? id.slice(0, SYNC_MAX_ID_CHARS) : null;
}

export function syncTitle(v: unknown): string {
  const cps = Array.from(cleanText(v));
  return cps.slice(0, SYNC_MAX_TITLE_CHARS).join("");
}

function envelope(raw: unknown, serverNow: Date, deviceId: string): SyncEnvelope | null {
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return null;
  const r = raw as Raw;
  const id = syncId(r.id);
  if (!id) return null;
  const updated_at = normalizeUpdatedAt(
    typeof r.updated_at === "string" ? r.updated_at : undefined,
    serverNow,
  );
  const created_at = normalizeUpdatedAt(
    typeof r.created_at === "string" ? r.created_at : updated_at,
    serverNow,
  );
  const deleted_at =
    typeof r.deleted_at === "string" && r.deleted_at
      ? normalizeUpdatedAt(r.deleted_at, serverNow)
      : null;
  return { id, created_at, updated_at, deleted_at, updated_by_device_id: deviceId };
}

export function normalizeSyncNote(
  raw: unknown,
  serverNow: Date,
  deviceId: string,
): { rec: SyncNote } | { skip: SyncSkip } {
  const env = envelope(raw, serverNow, deviceId);
  if (!env) return { skip: { id: rawId(raw), reason: "invalid" } };
  const r = raw as Raw;
  const body = cleanText(r.body);
  if (Buffer.byteLength(body, "utf8") > SYNC_MAX_NOTE_BODY_BYTES) {
    return { skip: { id: env.id, reason: "too_large" } };
  }
  return { rec: { ...env, title: syncTitle(r.title), body } };
}

export function normalizeSyncItems(v: unknown): ListItem[] {
  if (!Array.isArray(v)) return [];
  const out: ListItem[] = [];
  for (const it of v) {
    if (out.length >= SYNC_MAX_LIST_ITEMS) break;
    if (!it || typeof it !== "object") continue;
    const i = it as Raw;
    if (typeof i.id !== "string" || !i.id) continue;
    if (typeof i.text !== "string" || typeof i.checked !== "boolean") continue;
    if (typeof i.updated_at !== "string") continue;
    out.push({
      id: cleanText(i.id),
      text: cleanText(i.text),
      checked: i.checked,
      updated_at: cleanText(i.updated_at),
    });
  }
  return out;
}

export function normalizeSyncList(
  raw: unknown,
  serverNow: Date,
  deviceId: string,
): { rec: SyncList } | { skip: SyncSkip } {
  const env = envelope(raw, serverNow, deviceId);
  if (!env) return { skip: { id: rawId(raw), reason: "invalid" } };
  const r = raw as Raw;
  return { rec: { ...env, title: syncTitle(r.title), items: normalizeSyncItems(r.items) } };
}
