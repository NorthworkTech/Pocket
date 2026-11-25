/**
 * Wallpapers (pocket-contract v1.0 §B): 480×800 1-bit PBM (P4) + a layout with a live
 * clock box. Production mirror: neon-fn/scram-api.mjs.
 */
export const WALL_W = 480;
export const WALL_H = 800;
export const WALL_BYTES = (WALL_W / 8) * WALL_H; // 48,000
export const WALL_MAX_PER_DEVICE = 5;
const HEADER = Buffer.from(`P4\n${WALL_W} ${WALL_H}\n`);

export type WallLayout = {
  clock: { x: number; y: number; w: number; h: number; style: "large" | "small"; ink: "black" | "white" } | null;
  info: { show: boolean; ink: "black" | "white" };
};
export type WallDevices = "all" | string[];

/** Binary PBM (P4) → canonical bytes (header + 48,000), or null. */
export function parseWallPbm(b64: unknown): Buffer | null {
  if (typeof b64 !== "string" || b64.length > 70000) return null;
  const buf = Buffer.from(b64.replace(/^data:[^,]*,/, ""), "base64");
  const m = buf.subarray(0, 32).toString("latin1").match(/^P4\s+(\d+)\s+(\d+)\s/);
  if (!m || Number(m[1]) !== WALL_W || Number(m[2]) !== WALL_H) return null;
  const raster = buf.subarray(m[0].length);
  if (raster.length !== WALL_BYTES) return null;
  return Buffer.concat([HEADER, raster]);
}

export function parseWallLayout(layout: unknown): WallLayout | null {
  const out: WallLayout = { clock: null, info: { show: false, ink: "black" } };
  if (layout == null) return out;
  if (typeof layout !== "object" || Array.isArray(layout)) return null;
  const l = layout as Record<string, any>;
  const inkOk = (v: unknown) => v === undefined || v === "black" || v === "white";
  if (l.clock != null) {
    const c = l.clock;
    const n = Number.isInteger;
    if (typeof c !== "object" || !n(c.x) || !n(c.y) || !n(c.w) || !n(c.h)) return null;
    if (c.x < 0 || c.y < 0 || c.w < 120 || c.h < 48 || c.x + c.w > WALL_W || c.y + c.h > WALL_H) return null;
    if (!(c.style === undefined || c.style === "large" || c.style === "small") || !inkOk(c.ink)) return null;
    out.clock = { x: c.x, y: c.y, w: c.w, h: c.h, style: c.style || "large", ink: c.ink || "black" };
  }
  if (l.info != null) {
    const i = l.info;
    if (typeof i !== "object" || (i.show !== undefined && typeof i.show !== "boolean") || !inkOk(i.ink)) return null;
    out.info = { show: i.show === true, ink: i.ink || "black" };
  }
  return out;
}

export function readWallDevices(json: string): WallDevices {
  try {
    const d = JSON.parse(json || '"all"');
    return d === "all" ? "all" : Array.isArray(d) ? d.map(String) : "all";
  } catch {
    return "all";
  }
}

export function wallTargets(devicesJson: string, deviceId: string): boolean {
  const d = readWallDevices(devicesJson);
  return d === "all" || d.includes(deviceId);
}
