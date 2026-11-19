import { Hono } from "hono";
import type { AppEnv } from "../middleware/auth.js";
import { requireDeviceToken } from "../middleware/auth.js";
import { all, get, run } from "../db/index.js";
import { newId } from "../lib/crypto.js";
import { AppError, Errors } from "../lib/errors.js";
import {
  DEVICE_BACKUP_KEEP,
  DEVICE_BACKUP_MAX_BYTES,
  parseDeviceBackup,
  readBackupPayload,
} from "../lib/device-backup.js";

/**
 * Pocket device backup (settings, alarms, reading places). Production mirror: neon-fn/scram-api.mjs.
 *   POST /v1/device/backup        {settings,alarms,reading} (≤ 64 KB) → { ok, backup_id, created_at }
 *   GET  /v1/device/backup/latest → { backup: {id,created_at,device_id,device_name,settings,alarms,reading} } | 404
 * "latest" is across every Pocket on the account — that's how a new Pocket restores.
 */
export const deviceBackupRoutes = new Hono<AppEnv>();

deviceBackupRoutes.post("/backup", requireDeviceToken, async (c) => {
  const user = c.get("user")!;
  const link = c.get("deviceLink")!;
  const raw = await c.req.text().catch(() => "");
  if (Buffer.byteLength(raw, "utf8") > DEVICE_BACKUP_MAX_BYTES) {
    throw new AppError(413, "backup_too_large", "That backup is too large.");
  }
  let body: unknown = null;
  try {
    body = JSON.parse(raw);
  } catch {
    body = null;
  }
  const parsed = parseDeviceBackup(body);
  if (!parsed.ok) throw Errors.badRequest(parsed.message);
  const payload = JSON.stringify(parsed.value);
  const id = newId();
  const createdAt = new Date().toISOString();
  run(
    `INSERT INTO device_backups (id, user_id, device_id, device_name, created_at, payload_json, size_bytes)
     VALUES (?, ?, ?, ?, ?, ?, ?)`,
    [id, user.id, link.device_id, link.device_name || "Pocket", createdAt, payload, Buffer.byteLength(payload)],
  );
  const old = all<{ id: string }>(
    "SELECT id FROM device_backups WHERE user_id = ? ORDER BY created_at DESC, id DESC LIMIT -1 OFFSET ?",
    [user.id, DEVICE_BACKUP_KEEP],
  );
  for (const r of old) run("DELETE FROM device_backups WHERE id = ?", [r.id]);
  return c.json({ ok: true, backup_id: id, created_at: createdAt });
});

deviceBackupRoutes.get("/backup/latest", requireDeviceToken, (c) => {
  const user = c.get("user")!;
  const row = get<{
    id: string;
    created_at: string;
    device_id: string;
    device_name: string;
    payload_json: string;
  }>(
    `SELECT id, created_at, device_id, device_name, payload_json FROM device_backups
     WHERE user_id = ? ORDER BY created_at DESC, id DESC LIMIT 1`,
    [user.id],
  );
  if (!row) throw Errors.notFound("A backup");
  const p = readBackupPayload(row.payload_json);
  return c.json({
    backup: {
      id: row.id,
      created_at: row.created_at,
      device_id: row.device_id,
      device_name: row.device_name,
      settings: p.settings,
      alarms: p.alarms,
      reading: p.reading,
    },
  });
});
