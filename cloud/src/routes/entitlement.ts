import { Hono } from "hono";
import type { AppEnv } from "../middleware/auth.js";
import { requireSession, requireDeviceToken } from "../middleware/auth.js";
import { getEntitlementForUser } from "../lib/auth.js";
import { get, run } from "../db/index.js";

export const entitlementRoutes = new Hono<AppEnv>();

entitlementRoutes.get("/", requireSession, async (c) => {
  const user = c.get("user")!;
  const snapshot = getEntitlementForUser(user);
  return c.json({
    entitled: snapshot.entitled,
    status: snapshot.status,
    trial_ends_at: snapshot.trial_ends_at ?? null,
    current_period_end: snapshot.current_period_end ?? null,
    trial_consumed: snapshot.trial_consumed,
  });
});

export const deviceAttestRoutes = new Hono<AppEnv>();

deviceAttestRoutes.get("/attest", requireDeviceToken, async (c) => {
  const user = c.get("user")!;
  const snapshot = getEntitlementForUser(user);
  const link = c.get("deviceLink")!;
  // microSD report on the heartbeat: header x-pocket-sd: 1|0 or ?sd=1|0 (only when sent).
  const sdHeader = (c.req.header("x-pocket-sd") || "").trim();
  const sdParam = (c.req.query("sd") || "").trim();
  const sdRaw =
    sdHeader === "1" || sdHeader === "0"
      ? sdHeader
      : sdParam === "1" || sdParam === "0"
        ? sdParam
        : "";
  if (sdRaw) {
    run("UPDATE device_links SET sd_present = ? WHERE id = ?", [sdRaw === "1" ? 1 : 0, link.id]);
  }
  const sd = get<{ sd_present: number }>("SELECT sd_present FROM device_links WHERE id = ?", [
    link.id,
  ]);
  return c.json({
    entitled: snapshot.entitled,
    status: snapshot.status,
    trial_ends_at: snapshot.trial_ends_at ?? null,
    current_period_end: snapshot.current_period_end ?? null,
    trial_consumed: snapshot.trial_consumed,
    cloud_entitled: snapshot.entitled,
    device_id: link.device_id,
    device_name: link.device_name,
    sd_present: Boolean(sd?.sd_present),
  });
});
