import { createHash } from "node:crypto";
import { Hono } from "hono";
import type { Context } from "hono";
import type { AppEnv } from "../middleware/auth.js";
import { requireDeviceToken, requireSession } from "../middleware/auth.js";
import { all, get, run } from "../db/index.js";
import { newId } from "../lib/crypto.js";
import { AppError, Errors } from "../lib/errors.js";
import {
  WALL_MAX_PER_DEVICE,
  parseWallLayout,
  parseWallPbm,
  readWallDevices,
  wallTargets,
  type WallDevices,
} from "../lib/wallpapers.js";

/**
 * Wallpapers (pocket-contract v1.0 §B). Production mirror: neon-fn/scram-api.mjs.
 *   GET/POST /v1/wallpapers, GET /v1/wallpapers/:id/image.pbm, PATCH/DELETE /v1/wallpapers/:id
 *   GET /v1/device/wallpapers, GET /v1/device/wallpaper/:id   (device token)
 */
type Row = { id: string; name: string; sha256: string; layout_json: string; devices_json: string; created_at: string };
const COLS = "id, name, sha256, layout_json, devices_json, created_at";

function rowsFor(userId: string): Row[] {
  return all<Row>(`SELECT ${COLS} FROM wallpapers WHERE user_id = ? ORDER BY created_at DESC, id DESC`, [userId]);
}

function serialize(c: Context<AppEnv>, r: Row) {
  const origin = new URL(c.req.url).origin;
  return {
    id: r.id,
    name: r.name,
    sha256: r.sha256,
    created_at: r.created_at,
    devices: readWallDevices(r.devices_json),
    layout: JSON.parse(r.layout_json || "{}"),
    preview_url: `${origin}/v1/wallpapers/${r.id}/image.pbm`,
  };
}

function resolveDevices(userId: string, devices: unknown): WallDevices | null {
  if (devices === "all" || devices === undefined) return "all";
  if (!Array.isArray(devices) || devices.length === 0 || devices.length > 20) return null;
  const owned = new Set(all<{ device_id: string }>("SELECT device_id FROM device_links WHERE user_id = ?", [userId]).map((r) => r.device_id));
  const ids = [...new Set(devices.map(String))];
  return ids.every((d) => owned.has(d)) ? ids : null;
}

function overLimit(userId: string, rows: Row[], devices: WallDevices, exceptId: string | null): string[] {
  const owned = all<{ device_id: string }>("SELECT device_id FROM device_links WHERE user_id = ?", [userId]).map((r) => r.device_id);
  const targets = devices === "all" ? owned : devices;
  const others = rows.filter((r) => r.id !== exceptId);
  if (!targets.length) return others.length >= WALL_MAX_PER_DEVICE ? ["all"] : [];
  return targets.filter((d) => others.filter((r) => wallTargets(r.devices_json, d)).length >= WALL_MAX_PER_DEVICE);
}

const badName = () => new AppError(400, "bad_name", "Give the wallpaper a name (up to 40 characters).");
const notYours = () => new AppError(403, "device_not_yours", "Choose one of your own Pockets.");

export const wallpaperRoutes = new Hono<AppEnv>();

wallpaperRoutes.get("/", requireSession, (c) => {
  const user = c.get("user")!;
  return c.json({ wallpapers: rowsFor(user.id).map((r) => serialize(c, r)) });
});

wallpaperRoutes.post("/", requireSession, async (c) => {
  const user = c.get("user")!;
  const raw = await c.req.text().catch(() => "");
  if (raw.length > 80 * 1024) throw new AppError(413, "too_large", "That wallpaper is too large.");
  let body: Record<string, any> = {};
  try {
    body = JSON.parse(raw || "{}");
  } catch {
    throw Errors.badRequest("That request wasn't valid JSON.");
  }
  const name = typeof body.name === "string" ? body.name.trim() : "";
  if (!name || name.length > 40) throw badName();
  const pbm = parseWallPbm(body.image_pbm_b64);
  if (!pbm) throw new AppError(400, "bad_pbm", "The image must be a 480×800 black-and-white PBM (P4).");
  const layout = parseWallLayout(body.layout);
  if (!layout) throw new AppError(400, "bad_layout", "The layout isn't valid.");
  const devices = resolveDevices(user.id, body.devices);
  if (!devices) throw notYours();
  const rows = rowsFor(user.id);
  const full = overLimit(user.id, rows, devices, null);
  if (full.length) {
    return c.json(
      {
        message: "A Pocket can keep 5 wallpapers. Remove one first.",
        code: "too_many",
        devices: full,
        wallpapers: rows.map((r) => serialize(c, r)),
      },
      409,
    );
  }
  const row: Row = {
    id: "wp_" + newId().replace(/-/g, "").slice(0, 20),
    name,
    sha256: createHash("sha256").update(pbm).digest("hex"),
    layout_json: JSON.stringify(layout),
    devices_json: JSON.stringify(devices),
    created_at: new Date().toISOString(),
  };
  run(
    `INSERT INTO wallpapers (id, user_id, name, image_b64, sha256, layout_json, devices_json, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
    [row.id, user.id, row.name, pbm.toString("base64"), row.sha256, row.layout_json, row.devices_json, row.created_at],
  );
  return c.json({ ok: true, wallpaper: serialize(c, row) }, 201);
});

wallpaperRoutes.get("/:id/image.pbm", requireSession, (c) => {
  const user = c.get("user")!;
  const r = get<{ image_b64: string }>("SELECT image_b64 FROM wallpapers WHERE id = ? AND user_id = ?", [
    c.req.param("id") ?? "",
    user.id,
  ]);
  if (!r) throw Errors.notFound("That wallpaper");
  const bytes = Buffer.from(r.image_b64, "base64");
  return c.body(bytes, 200, { "content-type": "image/x-portable-bitmap", "cache-control": "private, max-age=300" });
});

wallpaperRoutes.patch("/:id", requireSession, async (c) => {
  const user = c.get("user")!;
  const rows = rowsFor(user.id);
  const row = rows.find((r) => r.id === c.req.param("id"));
  if (!row) throw Errors.notFound("That wallpaper");
  const body = (await c.req.json().catch(() => ({}))) as Record<string, any>;
  let name = row.name;
  if (body.name !== undefined) {
    name = typeof body.name === "string" ? body.name.trim() : "";
    if (!name || name.length > 40) throw badName();
  }
  let devices = readWallDevices(row.devices_json);
  if (body.devices !== undefined) {
    const d = resolveDevices(user.id, body.devices);
    if (!d) throw notYours();
    devices = d;
    const full = overLimit(user.id, rows, devices, row.id);
    if (full.length) {
      return c.json({ message: "A Pocket can keep 5 wallpapers. Remove one first.", code: "too_many", devices: full }, 409);
    }
  }
  run("UPDATE wallpapers SET name = ?, devices_json = ? WHERE id = ?", [name, JSON.stringify(devices), row.id]);
  return c.json({ ok: true, wallpaper: serialize(c, { ...row, name, devices_json: JSON.stringify(devices) }) });
});

wallpaperRoutes.delete("/:id", requireSession, (c) => {
  const user = c.get("user")!;
  const r = get<{ id: string }>("SELECT id FROM wallpapers WHERE id = ? AND user_id = ?", [c.req.param("id") ?? "", user.id]);
  if (!r) throw Errors.notFound("That wallpaper");
  run("DELETE FROM wallpapers WHERE id = ?", [r.id]);
  return c.json({ ok: true });
});

export const deviceWallpaperRoutes = new Hono<AppEnv>();

deviceWallpaperRoutes.get("/wallpapers", requireDeviceToken, (c) => {
  const user = c.get("user")!;
  const link = c.get("deviceLink")!;
  const rows = rowsFor(user.id)
    .filter((r) => wallTargets(r.devices_json, link.device_id))
    .slice(0, WALL_MAX_PER_DEVICE);
  return c.json({
    wallpapers: rows.map((r) => ({
      id: r.id,
      name: r.name,
      sha256: r.sha256,
      created_at: r.created_at,
      layout: JSON.parse(r.layout_json || "{}"),
    })),
  });
});

deviceWallpaperRoutes.get("/wallpaper/:id", requireDeviceToken, (c) => {
  const user = c.get("user")!;
  const link = c.get("deviceLink")!;
  const r = get<{ image_b64: string; devices_json: string }>(
    "SELECT image_b64, devices_json FROM wallpapers WHERE id = ? AND user_id = ?",
    [c.req.param("id") ?? "", user.id],
  );
  if (!r || !wallTargets(r.devices_json, link.device_id)) throw Errors.notFound("That wallpaper");
  return c.body(Buffer.from(r.image_b64, "base64"), 200, { "content-type": "application/octet-stream" });
});
