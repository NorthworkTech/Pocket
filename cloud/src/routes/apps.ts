import { Hono } from "hono";
import type { AppEnv } from "../middleware/auth.js";
import { requireDeviceToken, requireSession } from "../middleware/auth.js";
import { all, run } from "../db/index.js";
import { config } from "../config.js";
import { AppError } from "../lib/errors.js";
import { readWallDevices, wallTargets, type WallDevices } from "../lib/wallpapers.js";
import {
  APP_MAX_PER_DEVICE,
  FEED_MAX_BYTES,
  appUserId,
  authRow,
  parseAppFeed,
  registryApps,
  type AppManifest,
} from "../lib/apps.js";

/**
 * Apps (pocket-contract v1.0 §C–E). Production mirror: neon-fn/scram-api.mjs, which also runs
 * the registry poll and "Log in with <provider>" OAuth; this build serves the same shapes.
 *   GET  /v1/apps                → { installed, available }
 *   POST /v1/apps/install        { id, devices, return? } → { ok, needs_auth, auth_url? }
 *   POST /v1/apps/uninstall      { id }
 *   GET  /v1/device/apps         (device token) → { apps: [{ id, name, version, icon_pbm_b64, surfaces, status, feed, updated_at }] }
 */
type Install = { app_id: string; devices_json: string; installed_at: string; [k: string]: unknown };

function installsFor(userId: string): Install[] {
  return all<Install>(
    "SELECT app_id, devices_json, installed_at FROM app_installs WHERE user_id = ? ORDER BY installed_at",
    [userId],
  );
}

function resolveDevices(userId: string, devices: unknown): WallDevices | null {
  if (devices === "all" || devices === undefined) return "all";
  if (!Array.isArray(devices) || devices.length === 0 || devices.length > 20) return null;
  const owned = new Set(
    all<{ device_id: string }>("SELECT device_id FROM device_links WHERE user_id = ?", [userId]).map((r) => r.device_id),
  );
  const ids = [...new Set(devices.map(String))];
  return ids.every((d) => owned.has(d)) ? ids : null;
}

function authConnected(userId: string, app: AppManifest): boolean {
  if (app.auth.type !== "oauth2") return true;
  const a = authRow(userId, app.auth.provider);
  return Boolean(a && a.status === "ok");
}

export const appRoutes = new Hono<AppEnv>();

appRoutes.get("/", requireSession, (c) => {
  const user = c.get("user")!;
  const reg = registryApps();
  const installed = installsFor(user.id)
    .filter((r) => reg.has(r.app_id))
    .map((r) => {
      const app = reg.get(r.app_id)!;
      return {
        id: app.id,
        version: app.version,
        name: app.name,
        devices: readWallDevices(r.devices_json),
        auth_connected: authConnected(user.id, app),
      };
    });
  const available = [...reg.values()].map((a) => ({
    id: a.id,
    version: a.version,
    name: a.name,
    summary: a.summary,
    developer: a.developer,
    icon_pbm_b64: a.icon_pbm_b64,
    auth_type: a.auth.type,
    surfaces: a.surfaces,
  }));
  return c.json({ installed, available });
});

appRoutes.post("/install", requireSession, async (c) => {
  const user = c.get("user")!;
  const body = (await c.req.json().catch(() => ({}))) as Record<string, unknown>;
  const id = typeof body.id === "string" ? body.id : "";
  const app = registryApps().get(id);
  if (!app) throw new AppError(404, "not_found", "That app isn't available.");
  const devices = resolveDevices(user.id, body.devices);
  if (!devices) throw new AppError(403, "device_not_yours", "Choose one of your own Pockets.");
  const others = installsFor(user.id).filter((r) => r.app_id !== id);
  const owned = all<{ device_id: string }>("SELECT device_id FROM device_links WHERE user_id = ?", [user.id]).map(
    (r) => r.device_id,
  );
  const full = (devices === "all" ? owned : devices).filter(
    (d) => others.filter((r) => wallTargets(r.devices_json, d)).length >= APP_MAX_PER_DEVICE,
  );
  if (full.length) {
    return c.json({ message: "A Pocket can show 6 apps. Remove one first.", code: "too_many", devices: full }, 409);
  }
  run("DELETE FROM app_installs WHERE user_id = ? AND app_id = ?", [user.id, id]);
  run("INSERT INTO app_installs (user_id, app_id, devices_json, installed_at) VALUES (?, ?, ?, ?)", [
    user.id,
    id,
    JSON.stringify(devices),
    new Date().toISOString(),
  ]);
  if (!authConnected(user.id, app)) {
    // OAuth runs in the production function (it holds the provider client secrets).
    return c.json({ ok: true, needs_auth: true, message: "Sign-in for this app isn't available yet.", code: "auth_unavailable" });
  }
  return c.json({ ok: true, needs_auth: false });
});

appRoutes.post("/uninstall", requireSession, async (c) => {
  const user = c.get("user")!;
  const body = (await c.req.json().catch(() => ({}))) as Record<string, unknown>;
  const id = typeof body.id === "string" ? body.id : "";
  run("DELETE FROM app_installs WHERE user_id = ? AND app_id = ?", [user.id, id]);
  run("DELETE FROM app_feeds WHERE user_id = ? AND app_id = ?", [user.id, id]);
  return c.json({ ok: true });
});

async function fetchFeed(app: AppManifest, userId: string, token: string | null) {
  const headers: Record<string, string> = {
    accept: "application/json",
    "x-pocket-user": appUserId(userId, app, config.sessionSecret),
  };
  if (token) headers.authorization = `Bearer ${token}`;
  try {
    const res = await fetch(app.data_url, { headers, redirect: "error", signal: AbortSignal.timeout(8000) });
    if (!res.ok) return { error: `HTTP ${res.status}` };
    const raw = await res.text();
    if (Buffer.byteLength(raw) > FEED_MAX_BYTES) return { error: "too large" };
    const feed = parseAppFeed(JSON.parse(raw));
    return feed ? { feed } : { error: "invalid feed" };
  } catch (e) {
    return { error: String((e as Error)?.message || e).slice(0, 120) };
  }
}

export const deviceAppRoutes = new Hono<AppEnv>();

deviceAppRoutes.get("/apps", requireDeviceToken, async (c) => {
  const user = c.get("user")!;
  const link = c.get("deviceLink")!;
  const reg = registryApps();
  const installs = installsFor(user.id)
    .filter((r) => wallTargets(r.devices_json, link.device_id) && reg.has(r.app_id))
    .slice(0, APP_MAX_PER_DEVICE);
  const cached = new Map(
    all<{ app_id: string; feed_json: string | null; fetched_at: string; error: string | null }>(
      "SELECT app_id, feed_json, fetched_at, error FROM app_feeds WHERE user_id = ?",
      [user.id],
    ).map((r) => [r.app_id, r]),
  );
  const apps = [];
  for (const inst of installs) {
    const app = reg.get(inst.app_id)!;
    let status = "ok";
    let token: string | null = null;
    if (app.auth.type === "oauth2") {
      const a = authRow(user.id, app.auth.provider);
      if (!a) status = "connect";
      else if (a.status !== "ok") status = "reconnect";
      else if (Date.parse(a.expires_at) - Date.now() > 60_000) token = a.access_token;
      else status = "waiting"; // refresh happens in the production function
    }
    const prev = cached.get(app.id);
    const stale = !prev || Date.now() - Date.parse(prev.fetched_at) > app.refresh_minutes * 60_000;
    if (stale && status === "ok") {
      const r = await fetchFeed(app, user.id, token);
      const now = new Date().toISOString();
      const feedJson = r.feed ? JSON.stringify(r.feed) : prev?.feed_json ?? null;
      run("DELETE FROM app_feeds WHERE user_id = ? AND app_id = ?", [user.id, app.id]);
      run("INSERT INTO app_feeds (user_id, app_id, feed_json, fetched_at, error) VALUES (?, ?, ?, ?, ?)", [
        user.id,
        app.id,
        feedJson,
        now,
        r.error ?? null,
      ]);
      cached.set(app.id, { app_id: app.id, feed_json: feedJson, fetched_at: now, error: r.error ?? null });
    }
    const f = cached.get(app.id);
    let feed = null;
    try {
      feed = f?.feed_json ? JSON.parse(f.feed_json) : null;
    } catch {
      feed = null;
    }
    if (status === "ok" && !feed) status = "waiting";
    apps.push({
      id: app.id,
      name: app.name,
      version: app.version,
      icon_pbm_b64: app.icon_pbm_b64,
      surfaces: app.surfaces,
      status,
      feed,
      updated_at: f?.fetched_at ?? null,
    });
  }
  return c.json({ apps });
});
