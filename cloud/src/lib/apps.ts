/**
 * Pocket apps (pocket-contract v1.0 §C–E): manifest + feed validation, the signed registry
 * from northwork.dev, installs and feed delivery. Production mirror: neon-fn/scram-api.mjs.
 */
import { createHash, createHmac, timingSafeEqual } from "node:crypto";
import { all, get, run } from "../db/index.js";

export const APP_MAX_PER_DEVICE = 6;
export const FEED_MAX_BYTES = 16 * 1024;
export const APP_DEFAULT_RETURN = "https://northwork.app/pocket/app/apps";
const RETURN_HOSTS = new Set(["northwork.app", "www.northwork.app", "northwork.dev", "www.northwork.dev"]);
export const OAUTH_PROVIDERS: Record<string, { issuer: string; redirect: string }> = {
  guide: { issuer: "https://guidelearning.org", redirect: "https://northwork.app/pocket/app/oauth/guide/callback" },
};

export type AppManifest = {
  schema: "pocket-app/1";
  id: string;
  version: string;
  name: string;
  summary: string;
  developer: { id: string; name: string };
  icon_pbm_b64: string;
  data_url: string;
  refresh_minutes: number;
  auth: { type: "none" } | { type: "oauth2"; provider: string; scopes: string[] };
  surfaces: ("app" | "today")[];
  approved_at: string | null;
  review_id: string | null;
};
type Block =
  | { type: "header"; text: string }
  | { type: "kv"; key: string; value: string }
  | { type: "row"; text: string; detail?: string; go?: string }
  | { type: "text"; text: string }
  | { type: "empty"; text: string }
  | { type: "divider" };
export type AppFeed = {
  schema: "pocket-feed/1";
  title: string;
  today: string;
  screens: { id: string; title: string; blocks: Block[] }[];
  updated_at: string;
};

const strOk = (v: unknown, max: number, min = 1): v is string =>
  typeof v === "string" && v.length >= min && v.length <= max;
/** Plain text for the 1-bit fonts: no control chars, no astral-plane emoji. */
export const cleanText = (v: unknown) =>
  String(v)
    .replace(/[\u0000-\u0008\u000b-\u001f\u007f]/g, "")
    .replace(/[\u{10000}-\u{10FFFF}]/gu, "")
    .trim();

function iconOk(b64: unknown): b64 is string {
  if (typeof b64 !== "string" || b64.length > 400) return false;
  const buf = Buffer.from(b64, "base64");
  const m = buf.subarray(0, 16).toString("latin1").match(/^P4\s+32\s+32\s/);
  return Boolean(m) && buf.length - m![0].length === 128;
}

export function parseAppManifest(m: any): AppManifest | null {
  if (!m || typeof m !== "object" || m.schema !== "pocket-app/1") return null;
  if (typeof m.id !== "string" || !/^[a-z0-9](?:[a-z0-9.-]{0,62}[a-z0-9])?$/.test(m.id)) return null;
  if (typeof m.version !== "string" || !/^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$/.test(m.version)) return null;
  if (!strOk(m.name, 20) || (m.summary !== undefined && !strOk(m.summary, 80, 0))) return null;
  if (!m.developer || !strOk(m.developer.id, 64) || !strOk(m.developer.name, 64)) return null;
  if (!iconOk(m.icon_pbm_b64)) return null;
  let dataUrl: URL;
  try {
    dataUrl = new URL(m.data_url);
  } catch {
    return null;
  }
  if (dataUrl.protocol !== "https:") return null;
  const refresh = Number(m.refresh_minutes ?? 30);
  if (!Number.isInteger(refresh) || refresh < 15 || refresh > 1440) return null;
  let auth: AppManifest["auth"] = { type: "none" };
  if (m.auth && m.auth.type === "oauth2") {
    if (!OAUTH_PROVIDERS[m.auth.provider]) return null;
    const scopes = Array.isArray(m.auth.scopes)
      ? m.auth.scopes.filter((x: unknown) => typeof x === "string" && /^[\w.:-]{1,64}$/.test(x))
      : [];
    auth = { type: "oauth2", provider: m.auth.provider, scopes: scopes.slice(0, 20) };
  } else if (m.auth && m.auth.type !== "none") {
    return null;
  }
  const surfaces = (Array.isArray(m.surfaces) ? m.surfaces : ["app"]).filter(
    (x: unknown) => x === "app" || x === "today",
  ) as ("app" | "today")[];
  return {
    schema: "pocket-app/1",
    id: m.id,
    version: m.version,
    name: cleanText(m.name),
    summary: cleanText(m.summary || ""),
    developer: { id: String(m.developer.id), name: cleanText(m.developer.name) },
    icon_pbm_b64: m.icon_pbm_b64,
    data_url: dataUrl.toString(),
    refresh_minutes: refresh,
    auth,
    surfaces: surfaces.length ? [...new Set(surfaces)] : ["app"],
    approved_at: typeof m.approved_at === "string" ? m.approved_at : null,
    review_id: typeof m.review_id === "string" ? m.review_id : null,
  };
}

export function parseAppFeed(f: any): AppFeed | null {
  if (!f || typeof f !== "object" || f.schema !== "pocket-feed/1") return null;
  if (!strOk(f.title, 40)) return null;
  if (f.today !== undefined && f.today !== null && !strOk(f.today, 40, 0)) return null;
  if (!Array.isArray(f.screens) || f.screens.length < 1 || f.screens.length > 8) return null;
  const ids = new Set<string>();
  for (const sc of f.screens) {
    if (!sc || typeof sc.id !== "string" || !/^[A-Za-z0-9_-]{1,32}$/.test(sc.id) || ids.has(sc.id)) return null;
    ids.add(sc.id);
  }
  const screens: AppFeed["screens"] = [];
  for (const sc of f.screens) {
    if (!strOk(sc.title, 40) || !Array.isArray(sc.blocks) || sc.blocks.length > 40) return null;
    const blocks: Block[] = [];
    for (const b of sc.blocks) {
      if (!b || typeof b !== "object") return null;
      switch (b.type) {
        case "header":
          if (!strOk(b.text, 40)) return null;
          blocks.push({ type: "header", text: cleanText(b.text) });
          break;
        case "kv":
          if (!strOk(b.key, 24) || !strOk(b.value, 40, 0)) return null;
          blocks.push({ type: "kv", key: cleanText(b.key), value: cleanText(b.value) });
          break;
        case "row": {
          if (!strOk(b.text, 48) || (b.detail !== undefined && !strOk(b.detail, 24, 0))) return null;
          if (b.go !== undefined && !ids.has(b.go)) return null;
          const row: Block = { type: "row", text: cleanText(b.text) };
          if (b.detail) row.detail = cleanText(b.detail);
          if (b.go) row.go = b.go;
          blocks.push(row);
          break;
        }
        case "text":
          if (!strOk(b.text, 600)) return null;
          blocks.push({ type: "text", text: cleanText(b.text) });
          break;
        case "empty":
          if (!strOk(b.text, 80)) return null;
          blocks.push({ type: "empty", text: cleanText(b.text) });
          break;
        case "divider":
          blocks.push({ type: "divider" });
          break;
        default:
          break; // unknown block types from newer feeds are skipped
      }
    }
    screens.push({ id: sc.id, title: cleanText(sc.title), blocks });
  }
  return {
    schema: "pocket-feed/1",
    title: cleanText(f.title),
    today: f.today ? cleanText(f.today) : "",
    screens,
    updated_at: typeof f.updated_at === "string" ? f.updated_at : new Date().toISOString(),
  };
}

export function appReturnUrl(v: unknown): string {
  try {
    const u = new URL(String(v || ""));
    if (u.protocol === "https:" && RETURN_HOSTS.has(u.hostname)) return u.toString();
  } catch {
    /* fall through */
  }
  return APP_DEFAULT_RETURN;
}

/** Apply a registry response body if its X-Pocket-Signature (hex HMAC-SHA256) checks out. */
export function ingestRegistry(raw: string, signature: string, secret: string): boolean {
  const expect = createHmac("sha256", secret).update(raw).digest("hex");
  const sig = (signature || "").trim().toLowerCase();
  if (sig.length !== expect.length || !timingSafeEqual(Buffer.from(sig), Buffer.from(expect))) return false;
  let body: any;
  try {
    body = JSON.parse(raw);
  } catch {
    return false;
  }
  const revoked = new Set((Array.isArray(body.revoked) ? body.revoked : []).map(String));
  const keep = new Set<string>();
  const now = new Date().toISOString();
  for (const m of Array.isArray(body.apps) ? body.apps : []) {
    const app = parseAppManifest(m);
    if (!app || revoked.has(app.id) || revoked.has(`${app.id}@${app.version}`)) continue;
    keep.add(app.id);
    run("DELETE FROM app_registry WHERE id = ?", [app.id]);
    run("INSERT INTO app_registry (id, version, manifest_json, updated_at) VALUES (?, ?, ?, ?)", [
      app.id,
      app.version,
      JSON.stringify(app),
      now,
    ]);
  }
  for (const r of all<{ id: string }>("SELECT id FROM app_registry")) {
    if (!keep.has(r.id)) run("DELETE FROM app_registry WHERE id = ?", [r.id]);
  }
  return true;
}

export function registryApps(): Map<string, AppManifest> {
  const out = new Map<string, AppManifest>();
  for (const r of all<{ id: string; manifest_json: string }>("SELECT id, manifest_json FROM app_registry ORDER BY id")) {
    try {
      out.set(r.id, JSON.parse(r.manifest_json));
    } catch {
      /* skip */
    }
  }
  return out;
}

export function appUserId(userId: string, app: AppManifest, secret: string): string {
  return createHmac("sha256", secret).update(`${userId}:${app.developer.id}`).digest("hex").slice(0, 32);
}

export function appsRev(parts: string[]): string {
  return createHash("sha256").update(parts.join("|")).digest("hex").slice(0, 16);
}

export function authRow(userId: string, provider: string) {
  return get<{ access_token: string; refresh_token: string | null; expires_at: string; status: string }>(
    "SELECT access_token, refresh_token, expires_at, status FROM app_auth WHERE user_id = ? AND provider = ?",
    [userId, provider],
  );
}
