/**
 * Pocket device backups (settings, alarms, reading places) — validation and summaries.
 * Production mirror: neon-fn/scram-api.mjs (keep both in step).
 */

export const DEVICE_BACKUP_MAX_BYTES = 64 * 1024;
export const DEVICE_BACKUP_KEEP = 5;
/** A "Back up now" request from Companion is delivered once on attest, or dropped after this. */
export const BACKUP_REQUEST_TTL_MS = 24 * 3600 * 1000;

export type DeviceBackupPayload = {
  settings: Record<string, unknown>;
  alarms: unknown[];
  reading: Record<string, unknown>;
};

const isObj = (v: unknown): v is Record<string, unknown> =>
  !!v && typeof v === "object" && !Array.isArray(v);

/** POST /v1/device/backup body → payload, or an error message. */
export function parseDeviceBackup(
  body: unknown,
): { ok: true; value: DeviceBackupPayload } | { ok: false; message: string } {
  const bad = { ok: false as const, message: "That backup couldn't be read." };
  if (!isObj(body)) return bad;
  const { settings = {}, alarms = [], reading = {} } = body;
  if (!isObj(settings) || !Array.isArray(alarms) || !isObj(reading)) return bad;
  if (body.settings === undefined && body.alarms === undefined && body.reading === undefined) return bad;
  return { ok: true, value: { settings, alarms, reading } };
}

/** How many reading places a backup holds: `positions`/`books` arrays, else one per key. */
export function readingCount(reading: Record<string, unknown>): number {
  if (Array.isArray(reading.positions)) return reading.positions.length;
  if (Array.isArray(reading.books)) return reading.books.length;
  return Object.keys(reading).length;
}

export function backupSummary(payload: DeviceBackupPayload) {
  return {
    settings_count: Object.keys(payload.settings).length,
    alarm_count: payload.alarms.length,
    reading_count: readingCount(payload.reading),
  };
}

export function readBackupPayload(json: string): DeviceBackupPayload {
  try {
    const parsed = parseDeviceBackup(JSON.parse(json));
    if (parsed.ok) return parsed.value;
  } catch {
    // fall through
  }
  return { settings: {}, alarms: [], reading: {} };
}

export function backupRequestIsFresh(requestedAt: string | null | undefined, now: Date): boolean {
  if (!requestedAt) return false;
  const t = Date.parse(requestedAt);
  return Number.isFinite(t) && now.getTime() - t <= BACKUP_REQUEST_TTL_MS;
}
