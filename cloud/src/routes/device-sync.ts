import { Hono } from "hono";
import type { AppEnv } from "../middleware/auth.js";
import { requireDeviceToken } from "../middleware/auth.js";
import { all, get, run } from "../db/index.js";
import { AppError, Errors } from "../lib/errors.js";
import { incomingWinsLww } from "../lib/lww.js";
import {
  SYNC_MAX_RECORDS,
  SYNC_TOMBSTONE_DAYS,
  normalizeSyncList,
  normalizeSyncNote,
  type SyncEnvelope,
  type SyncList,
  type SyncNote,
  type SyncSkip,
} from "../lib/device-sync.js";
import { serializeNote, type NoteRow } from "./notes.js";
import { serializeList, type ListRow } from "./lists.js";
import { LIST_ACCESS_SQL, canAccessList, displayName } from "../lib/family.js";

type ExistingRow = {
  id: string;
  user_id: string;
  updated_at: string;
  deleted_at: string | null;
  updated_by_device_id: string;
  family_id?: string | null;
};

/**
 * Device ↔ cloud notes/lists sync. Body carries only records changed on the device since its
 * last good sync (tombstones have deleted_at); the response is the user's full live set plus
 * tombstones from the last 30 days. Production mirror: neon-fn/scram-api.mjs.
 */
export const deviceSyncRoutes = new Hono<AppEnv>();

function applyRecords<T extends SyncEnvelope>(
  table: "cloud_notes" | "cloud_lists",
  recs: T[],
  userId: string,
  fields: (rec: T) => Record<string, string>,
  skipped: SyncSkip[],
): void {
  const existing = new Map<string, ExistingRow>();
  for (const r of recs) {
    if (existing.has(r.id)) continue;
    const row = get<ExistingRow>(
      table === "cloud_lists"
        ? `SELECT id, user_id, updated_at, deleted_at, updated_by_device_id, family_id FROM cloud_lists WHERE id = ?`
        : `SELECT id, user_id, updated_at, deleted_at, updated_by_device_id FROM cloud_notes WHERE id = ?`,
      [r.id],
    );
    if (row) existing.set(r.id, row);
  }
  for (const r of recs) {
    const f = fields(r);
    const cols = Object.keys(f);
    const ex = existing.get(r.id);
    const next: ExistingRow = {
      id: r.id,
      user_id: userId,
      updated_at: r.updated_at,
      deleted_at: r.deleted_at,
      updated_by_device_id: r.updated_by_device_id,
    };
    if (!ex) {
      run(
        `INSERT OR IGNORE INTO ${table}
          (id, user_id, ${cols.join(", ")}, created_at, updated_at, updated_by_device_id, deleted_at, version)
         VALUES (?, ?, ${cols.map(() => "?").join(", ")}, ?, ?, ?, ?, 1)`,
        [
          r.id,
          userId,
          ...cols.map((c) => f[c]!),
          r.created_at,
          r.updated_at,
          r.updated_by_device_id,
          r.deleted_at,
        ],
      );
      existing.set(r.id, next);
      continue;
    }
    // Someone else's record: only a list shared into this user's Pocket Family is writable, and
    // only its owner may delete it.
    const familyEdit =
      ex.user_id !== userId &&
      table === "cloud_lists" &&
      canAccessList({ user_id: ex.user_id, family_id: ex.family_id ?? null }, userId);
    if (ex.user_id !== userId && !familyEdit) {
      skipped.push({ id: r.id, reason: "unavailable" });
      continue;
    }
    if (familyEdit && r.deleted_at) {
      skipped.push({ id: r.id, reason: "not_owner" });
      continue;
    }
    if (!incomingWinsLww(r, ex)) continue; // cloud copy is newer; it comes back in the response
    run(
      `UPDATE ${table} SET ${cols.map((c) => `${c} = ?`).join(", ")}, updated_at = ?, deleted_at = ?,
         updated_by_device_id = ?, version = version + 1
       WHERE id = ? AND user_id = ?`,
      [
        ...cols.map((c) => f[c]!),
        r.updated_at,
        r.deleted_at,
        r.updated_by_device_id,
        r.id,
        ex.user_id,
      ],
    );
    existing.set(r.id, { ...ex, ...next, user_id: ex.user_id });
  }
}

deviceSyncRoutes.post("/sync", requireDeviceToken, async (c) => {
  const user = c.get("user")!;
  const link = c.get("deviceLink")!;
  const deviceId = link.device_id;

  const body = (await c.req.json().catch(() => null)) as Record<string, unknown> | null;
  if (!body || typeof body !== "object" || Array.isArray(body)) {
    throw Errors.badRequest("That sync request couldn't be read.");
  }
  const rawNotes = body.notes == null ? [] : body.notes;
  const rawLists = body.lists == null ? [] : body.lists;
  if (!Array.isArray(rawNotes) || !Array.isArray(rawLists)) {
    throw Errors.badRequest("That sync request couldn't be read.");
  }
  if (rawNotes.length > SYNC_MAX_RECORDS || rawLists.length > SYNC_MAX_RECORDS) {
    throw new AppError(
      413,
      "too_many_records",
      "Too many changes in one sync. Send them in smaller batches.",
    );
  }

  const serverNow = new Date();
  // requireDeviceToken already stamped last_seen_at.
  if (typeof body.sd_present === "boolean") {
    run("UPDATE device_links SET sd_present = ? WHERE id = ?", [body.sd_present ? 1 : 0, link.id]);
  }

  if (!c.get("entitlement")?.entitled) {
    const message = "Pocket Cloud is needed to sync notes and lists.";
    return c.json({ message, entitled: false, error: { code: "not_entitled", message } }, 402);
  }

  const skipped: { notes: SyncSkip[]; lists: SyncSkip[] } = { notes: [], lists: [] };
  const notes: SyncNote[] = [];
  for (const raw of rawNotes) {
    const r = normalizeSyncNote(raw, serverNow, deviceId);
    if ("rec" in r) notes.push(r.rec);
    else skipped.notes.push(r.skip);
  }
  const lists: SyncList[] = [];
  for (const raw of rawLists) {
    const r = normalizeSyncList(raw, serverNow, deviceId);
    if ("rec" in r) lists.push(r.rec);
    else skipped.lists.push(r.skip);
  }
  applyRecords("cloud_notes", notes, user.id, (n) => ({ title: n.title, body: n.body }), skipped.notes);
  applyRecords(
    "cloud_lists",
    lists,
    user.id,
    (l) => ({ title: l.title, items_json: JSON.stringify(l.items) }),
    skipped.lists,
  );

  const cutoff = new Date(serverNow.getTime() - SYNC_TOMBSTONE_DAYS * 86400000).toISOString();
  const noteRows = all<NoteRow>(
    `SELECT * FROM cloud_notes WHERE user_id = ? AND (deleted_at IS NULL OR deleted_at > ?)
     ORDER BY updated_at DESC`,
    [user.id, cutoff],
  );
  // Own lists plus lists shared into the user's Pocket Family (shared ones carry the owner's name).
  const listRows = all<ListRow & { owner_email: string }>(
    `SELECT l.*, u.email AS owner_email FROM cloud_lists l JOIN users u ON u.id = l.user_id
     WHERE ${LIST_ACCESS_SQL} AND (l.deleted_at IS NULL OR l.deleted_at > ?)
     ORDER BY l.updated_at DESC`,
    [user.id, user.id, cutoff],
  );
  const sd = get<{ sd_present: number }>("SELECT sd_present FROM device_links WHERE id = ?", [
    link.id,
  ]);
  return c.json({
    server_time: serverNow.toISOString(),
    sd_present: Boolean(sd?.sd_present),
    notes: noteRows.map(serializeNote),
    lists: listRows.map((row) =>
      row.family_id
        ? { ...serializeList(row), shared: true, owner: displayName(row.owner_email) }
        : { ...serializeList(row), shared: false },
    ),
    skipped,
  });
});
