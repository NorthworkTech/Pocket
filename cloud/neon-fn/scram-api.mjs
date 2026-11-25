import { connect } from "tls";
import { createHash, createHmac, pbkdf2Sync, randomBytes, randomUUID, scryptSync, timingSafeEqual } from "crypto";
import { inflateRawSync } from "zlib";

const DEVICE_API_KEY = (process.env.DEVICE_API_KEY || "dev-device-api-key").trim();
const SESSION_COOKIE = process.env.SESSION_COOKIE_NAME || "pocket_session";
const PWA_ORIGIN = (process.env.PWA_ORIGIN || "https://northwork.app/pocket/app").replace(/\/$/, "");
const PUBLIC_BASE_URL = (process.env.PUBLIC_BASE_URL || "").replace(/\/$/, "");
const sha256Hex = (i) => createHash("sha256").update(i, "utf8").digest("hex");
const randomToken = (b = 32) => randomBytes(b).toString("base64url");
const newId = () => randomUUID();
function hashPassword(p) {
  const s = randomBytes(16).toString("hex");
  return `scrypt$${s}$${scryptSync(p, s, 64).toString("hex")}`;
}
function verifyPassword(p, st) {
  const parts = String(st || "").split("$");
  if (parts.length !== 3 || parts[0] !== "scrypt") return false;
  const a = scryptSync(p, parts[1], 64).toString("hex");
  try {
    return timingSafeEqual(Buffer.from(a, "hex"), Buffer.from(parts[2], "hex"));
  } catch {
    return false;
  }
}
const PAIR_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const normalizePairCode = (c) => c.trim().toUpperCase().replace(/[^A-Z2-9]/g, "");
const isValidPairCode = (c) => {
  const n = normalizePairCode(c);
  return n.length === 8 && [...n].every((ch) => PAIR_ALPHABET.includes(ch));
};
function safeEqual(a, b) {
  const ba = Buffer.from(String(a)), bb = Buffer.from(String(b));
  if (ba.length !== bb.length) return false;
  return timingSafeEqual(ba, bb);
}
function cors(req) {
  const o = req.headers.get("origin") || "";
  const h = { vary: "Origin" };
  if (o) {
    h["access-control-allow-origin"] = o;
    h["access-control-allow-credentials"] = "true";
    // x-pocket-session: bearer alternative — third-party cookies often blocked on mobile Safari / ITP.
    h["access-control-allow-headers"] = "content-type,authorization,x-device-key,x-pocket-session,x-pcm-rate,x-pcm-encoding,x-device-id,x-device-token,x-pocket-token";
    h["access-control-allow-methods"] = "GET,POST,PUT,PATCH,DELETE,OPTIONS";
  }
  return h;
}
function json(req, body, status = 200, extra = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { ...cors(req), "content-type": "application/json", ...extra },
  });
}
function getCookie(req, name) {
  const c = req.headers.get("cookie") || "";
  const m = c.match(new RegExp("(?:^|;\\s*)" + name + "=([^;]+)"));
  return m ? decodeURIComponent(m[1]) : null;
}
function sessionCookie(token, maxAge) {
  return `${SESSION_COOKIE}=${encodeURIComponent(token)}; Path=/; HttpOnly; Secure; SameSite=None; Max-Age=${maxAge}`;
}
function parseDbUrl(u) {
  const x = new URL(u);
  return {
    host: x.hostname,
    port: Number(x.port || 5432),
    user: decodeURIComponent(x.username),
    pass: decodeURIComponent(x.password),
    db: x.pathname.slice(1),
  };
}
function encodeStartup({ user, db }) {
  const params = Buffer.from(`user\0${user}\0database\0${db}\0\0`);
  const len = 8 + params.length;
  const b = Buffer.alloc(len);
  b.writeInt32BE(len, 0);
  b.writeInt32BE(196608, 4);
  params.copy(b, 8);
  return b;
}
function encodePassword(pw) {
  const body = Buffer.from(pw + "\0");
  const b = Buffer.alloc(5 + body.length);
  b[0] = 0x70;
  b.writeInt32BE(4 + body.length, 1);
  body.copy(b, 5);
  return b;
}
function encodeSaslInitial(mechanism, data) {
  const mech = Buffer.from(mechanism + "\0");
  const d = Buffer.from(data, "utf8");
  const len = 4 + mech.length + 4 + d.length;
  const b = Buffer.alloc(1 + len);
  b[0] = 0x70;
  b.writeInt32BE(len, 1);
  mech.copy(b, 5);
  b.writeInt32BE(d.length, 5 + mech.length);
  d.copy(b, 5 + mech.length + 4);
  return b;
}
function encodeSaslResponse(data) {
  const d = Buffer.from(data, "utf8");
  const b = Buffer.alloc(5 + d.length);
  b[0] = 0x70;
  b.writeInt32BE(4 + d.length, 1);
  d.copy(b, 5);
  return b;
}
function encodeQuery(sql) {
  const body = Buffer.from(sql + "\0");
  const b = Buffer.alloc(5 + body.length);
  b[0] = 0x51;
  b.writeInt32BE(4 + body.length, 1);
  body.copy(b, 5);
  return b;
}
function esc(s) {
  return String(s).replace(/'/g, "''");
}
function sqlStr(s) {
  return "'" + esc(s) + "'";
}
function sqlNullable(s) {
  return s == null || s === "" ? "NULL" : sqlStr(s);
}
function scramFinal(password, clientFirstBare, serverFirst, clientNonce) {
  const kv = Object.fromEntries(
    serverFirst.split(",").map((p) => {
      const i = p.indexOf("=");
      return [p.slice(0, i), p.slice(i + 1)];
    })
  );
  if (!kv.r || !kv.r.startsWith(clientNonce)) throw new Error("scram nonce mismatch");
  const salt = Buffer.from(kv.s, "base64");
  const iter = parseInt(kv.i, 10);
  const salted = pbkdf2Sync(password, salt, iter, 32, "sha256");
  const clientKey = createHmac("sha256", salted).update("Client Key").digest();
  const storedKey = createHash("sha256").update(clientKey).digest();
  const withoutProof = `c=biws,r=${kv.r}`;
  const authMsg = `${clientFirstBare},${serverFirst},${withoutProof}`;
  const clientSig = createHmac("sha256", storedKey).update(authMsg).digest();
  const proof = Buffer.alloc(clientKey.length);
  for (let i = 0; i < clientKey.length; i++) proof[i] = clientKey[i] ^ clientSig[i];
  return `${withoutProof},p=${proof.toString("base64")}`;
}

let sock = null, buf = Buffer.alloc(0), waiters = [], q = [];
function feed(chunk) {
  buf = Buffer.concat([buf, chunk]);
  while (buf.length >= 5) {
    const type = String.fromCharCode(buf[0]);
    const len = buf.readInt32BE(1);
    if (buf.length < 1 + len) break;
    const payload = buf.subarray(5, 1 + len);
    buf = buf.subarray(1 + len);
    const msg = { type, payload };
    if (waiters.length) waiters.shift()(msg);
    else q.push(msg);
  }
}
function once() {
  return new Promise((resolve, reject) => {
    if (q.length) return resolve(q.shift());
    const t = setTimeout(() => reject(new Error("pg timeout")), 15000);
    waiters.push((msg) => {
      clearTimeout(t);
      resolve(msg);
    });
  });
}
async function ensurePg() {
  if (sock) return;
  const cfg = parseDbUrl(process.env.DATABASE_URL);
  sock = await new Promise((res, rej) => {
    const s = connect(
      { host: cfg.host, port: cfg.port, servername: cfg.host, rejectUnauthorized: false },
      () => res(s)
    );
    s.on("error", (e) => {
      sock = null;
      rej(e);
    });
  });
  sock.on("data", feed);
  sock.on("close", () => { sock = null; buf = Buffer.alloc(0); waiters = []; q = []; });
  sock.write(encodeStartup({ user: cfg.user, db: cfg.db }));
  let clientNonce = "", clientFirstBare = "";
  for (;;) {
    const { type, payload } = await once();
    if (type === "R") {
      const kind = payload.readInt32BE(0);
      if (kind === 0) continue;
      if (kind === 3) { sock.write(encodePassword(cfg.pass)); continue; }
      if (kind === 10) {
        const mechs = payload.slice(4).toString("utf8");
        if (!mechs.includes("SCRAM-SHA-256")) throw new Error("no scram: " + mechs);
        clientNonce = randomBytes(18).toString("base64");
        clientFirstBare = `n=${cfg.user},r=${clientNonce}`;
        sock.write(encodeSaslInitial("SCRAM-SHA-256", `n,,${clientFirstBare}`));
        continue;
      }
      if (kind === 11) {
        const serverFirst = payload.slice(4).toString("utf8");
        sock.write(encodeSaslResponse(scramFinal(cfg.pass, clientFirstBare, serverFirst, clientNonce)));
        continue;
      }
      if (kind === 12) continue;
      throw new Error("unsupported auth " + kind);
    }
    if (type === "Z") break;
    if (type === "E") throw new Error(payload.toString());
  }
}
async function query(sql) {
  await ensurePg();
  const rows = [];
  let fields = [];
  sock.write(encodeQuery(sql));
  for (;;) {
    const { type, payload } = await once();
    if (type === "T") {
      let n = payload.readInt16BE(0), o = 2;
      fields = [];
      for (let i = 0; i < n; i++) {
        let e = payload.indexOf(0, o);
        fields.push(payload.toString("utf8", o, e));
        o = e + 1 + 18;
      }
    } else if (type === "D") {
      let n = payload.readInt16BE(0), o = 2;
      const row = {};
      for (let i = 0; i < n; i++) {
        const l = payload.readInt32BE(o);
        o += 4;
        row[fields[i]] = l === -1 ? null : payload.toString("utf8", o, o + l);
        if (l !== -1) o += l;
      }
      rows.push(row);
    } else if (type === "C") {
    } else if (type === "Z") return rows;
    else if (type === "E") throw new Error(payload.toString("utf8"));
  }
}
async function userFromSession(req) {
  // Prefer explicit header (works cross-site when cookies are blocked on mobile).
  const headerTok = (req.headers.get("x-pocket-session") || "").trim();
  const cookieTok = getCookie(req, SESSION_COOKIE);
  const token = headerTok || cookieTok;
  if (!token) return null;
  const sessions = await query(`SELECT user_id, expires_at FROM sessions WHERE token_hash='${esc(sha256Hex(token))}'`);
  const session = sessions[0];
  if (!session) return null;
  if (Date.parse(session.expires_at) < Date.now()) {
    await query(`DELETE FROM sessions WHERE token_hash='${esc(sha256Hex(token))}'`);
    return null;
  }
  const users = await query(`SELECT * FROM users WHERE id='${esc(session.user_id)}'`);
  return users[0] || null;
}


const ADMIN_EMAILS = new Set(
  (process.env.ADMIN_EMAILS || "hillelfrankel0@icloud.com")
    .split(",")
    .map((s) => s.trim().toLowerCase())
    .filter(Boolean),
);

/** Northwork sign-in: northwork.app signs a short-lived pass {sub,email,name,aud:"pocket",exp(ms)}
 * as base64url(json) + "." + base64url(HMAC-SHA256(body, NORTHWORK_SSO_SECRET)). */
function verifyNorthworkPass(token) {
  const secret = (process.env.NORTHWORK_SSO_SECRET || "").trim();
  if (!secret || typeof token !== "string") return null;
  const [body, sig] = token.split(".");
  if (!body || !sig) return null;
  const expect = createHmac("sha256", secret).update(body).digest("base64url");
  if (!safeEqual(expect, sig)) return null;
  let p;
  try {
    p = JSON.parse(Buffer.from(body, "base64url").toString("utf8"));
  } catch {
    return null;
  }
  if (!p || p.aud !== "pocket" || typeof p.exp !== "number" || p.exp < Date.now()) return null;
  if (typeof p.email !== "string" || !p.email.includes("@")) return null;
  return p;
}

/** Without a local NORTHWORK_SSO_SECRET, ask northwork.app whether the pass is genuine
 * (the signing secret then never leaves the Northwork site). */
async function checkNorthworkPass(token) {
  if ((process.env.NORTHWORK_SSO_SECRET || "").trim()) return verifyNorthworkPass(token);
  if (typeof token !== "string" || token.length > 4096) return null;
  const base = (process.env.NORTHWORK_URL || "https://northwork.app").replace(/\/$/, "");
  try {
    const res = await globalThis.fetch(`${base}/api/integrations/pocket/verify`, {
      method: "POST",
      headers: { "content-type": "application/json", "user-agent": "Pocket-Cloud/1.0" },
      body: JSON.stringify({ token }),
      signal: AbortSignal.timeout(8000),
    });
    if (!res.ok) return null;
    const data = await res.json().catch(() => null);
    const u = data && data.ok ? data.user : null;
    if (!u || typeof u.email !== "string" || !u.email.includes("@")) return null;
    return { sub: String(u.id || ""), email: u.email, name: u.name || "" };
  } catch {
    return null;
  }
}

function isAdminUser(user) {
  if (!user) return false;
  if (String(user.role || "") === "admin") return true;
  return ADMIN_EMAILS.has(String(user.email || "").toLowerCase());
}

async function requireSessionUser(req) {
  const user = await userFromSession(req);
  if (!user) return { error: json(req, { message: "Sign in to continue." }, 401) };
  if (user.disabled_at) return { error: json(req, { message: "This account is disabled." }, 403) };
  return { user };
}

async function requireEntitledUser(req) {
  const gate = await requireSessionUser(req);
  if (gate.error) return gate;
  const subs = await query(
    `SELECT status FROM subscription_mirrors WHERE user_id='${esc(gate.user.id)}'`,
  );
  const st = subs[0]?.status;
  if (st !== "active" && st !== "trialing") {
    return { error: json(req, { message: "Pocket Cloud is required for this.", code: "not_entitled" }, 403) };
  }
  return gate;
}

async function requireAdmin(req) {
  const gate = await requireSessionUser(req);
  if (gate.error) return gate;
  if (!isAdminUser(gate.user)) return { error: json(req, { message: "Admin access required." }, 403) };
  return gate;
}

function serializeNote(row) {
  return {
    id: row.id,
    title: row.title || "",
    body: row.body || "",
    created_at: row.created_at,
    updated_at: row.updated_at,
    updated_by_device_id: row.updated_by_device_id,
    deleted_at: row.deleted_at || null,
    version: Number(row.version || 1),
  };
}

function serializeList(row) {
  let items = [];
  try {
    items = JSON.parse(row.items_json || "[]");
    if (!Array.isArray(items)) items = [];
  } catch {
    items = [];
  }
  return {
    id: row.id,
    title: row.title || "",
    items,
    created_at: row.created_at,
    updated_at: row.updated_at,
    updated_by_device_id: row.updated_by_device_id,
    deleted_at: row.deleted_at || null,
    version: Number(row.version || 1),
  };
}

// ---- Device ↔ cloud notes/lists sync (POST /v1/device/sync) ----
// Ported from cloud/src/lib/lww.ts + cloud/src/lib/device-sync.ts — keep them in step.
const SYNC_MAX_RECORDS = 1000; // per kind (notes / lists) per request
const SYNC_MAX_LIST_ITEMS = 500;
const SYNC_MAX_TITLE_CHARS = 300;
const SYNC_MAX_NOTE_BODY_BYTES = 200 * 1024;
const SYNC_MAX_ID_CHARS = 200;
const SYNC_TOMBSTONE_DAYS = 30;

/** Clamp future client clocks: >1h ahead of server ⇒ server now. Invalid/missing ⇒ server now. */
function normalizeUpdatedAt(clientIso, serverNow) {
  if (!clientIso) return serverNow.toISOString();
  const t = Date.parse(clientIso);
  if (!Number.isFinite(t)) return serverNow.toISOString();
  if (t - serverNow.getTime() > 60 * 60 * 1000) return serverNow.toISOString();
  return new Date(t).toISOString();
}

/** Conflict instant = max(deleted_at, updated_at) — a newer tombstone beats an older edit. */
function lwwConflictInstant(row) {
  const tombstone = row.deleted_at ? Date.parse(row.deleted_at) : NaN;
  const updated = Date.parse(row.updated_at);
  if (Number.isFinite(tombstone) && Number.isFinite(updated)) return Math.max(tombstone, updated);
  if (Number.isFinite(tombstone)) return tombstone;
  if (Number.isFinite(updated)) return updated;
  return 0;
}

/** True when `incoming` should replace `existing` (tie ⇒ higher updated_by_device_id). */
function incomingWinsLww(incoming, existing) {
  const a = lwwConflictInstant(incoming);
  const b = lwwConflictInstant(existing);
  if (a > b) return true;
  if (a < b) return false;
  return String(incoming.updated_by_device_id || "") > String(existing.updated_by_device_id || "");
}

/** NUL can't live in a Postgres text value (and would cut a simple-query message short). */
function syncCleanText(v) {
  return typeof v === "string" ? v.replace(/\u0000/g, "") : "";
}

function syncId(v) {
  if (typeof v !== "string") return null;
  const s = v.trim();
  if (!s || s.length > SYNC_MAX_ID_CHARS || s.includes("\u0000")) return null;
  return s;
}

function syncTitle(v) {
  const s = syncCleanText(v);
  const cps = Array.from(s);
  return cps.length > SYNC_MAX_TITLE_CHARS ? cps.slice(0, SYNC_MAX_TITLE_CHARS).join("") : s;
}

/** Shared envelope (id + timestamps) for an incoming note/list. Returns null when unusable. */
function syncEnvelope(raw, serverNow, deviceId) {
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return null;
  const id = syncId(raw.id);
  if (!id) return null;
  const updated_at = normalizeUpdatedAt(typeof raw.updated_at === "string" ? raw.updated_at : undefined, serverNow);
  const created_at = normalizeUpdatedAt(typeof raw.created_at === "string" ? raw.created_at : updated_at, serverNow);
  const deleted_at =
    typeof raw.deleted_at === "string" && raw.deleted_at
      ? normalizeUpdatedAt(raw.deleted_at, serverNow)
      : null;
  return { id, created_at, updated_at, deleted_at, updated_by_device_id: deviceId };
}

function syncNormalizeNote(raw, serverNow, deviceId) {
  const env = syncEnvelope(raw, serverNow, deviceId);
  if (!env) return { skip: { id: typeof raw?.id === "string" ? raw.id.slice(0, SYNC_MAX_ID_CHARS) : null, reason: "invalid" } };
  const body = syncCleanText(raw.body);
  if (Buffer.byteLength(body, "utf8") > SYNC_MAX_NOTE_BODY_BYTES) {
    return { skip: { id: env.id, reason: "too_large" } };
  }
  return { rec: { ...env, fields: { title: syncTitle(raw.title), body } } };
}

function syncNormalizeItems(v) {
  if (!Array.isArray(v)) return [];
  const out = [];
  for (const it of v) {
    if (out.length >= SYNC_MAX_LIST_ITEMS) break;
    if (!it || typeof it !== "object") continue;
    if (typeof it.id !== "string" || !it.id) continue;
    if (typeof it.text !== "string" || typeof it.checked !== "boolean" || typeof it.updated_at !== "string") continue;
    out.push({
      id: syncCleanText(it.id),
      text: syncCleanText(it.text),
      checked: it.checked,
      updated_at: syncCleanText(it.updated_at),
    });
  }
  return out;
}

function syncNormalizeList(raw, serverNow, deviceId) {
  const env = syncEnvelope(raw, serverNow, deviceId);
  if (!env) return { skip: { id: typeof raw?.id === "string" ? raw.id.slice(0, SYNC_MAX_ID_CHARS) : null, reason: "invalid" } };
  return {
    rec: {
      ...env,
      fields: { title: syncTitle(raw.title), items_json: JSON.stringify(syncNormalizeItems(raw.items)) },
    },
  };
}

/**
 * Apply device records to cloud_notes / cloud_lists with LWW. `table` and the field column
 * names are fixed identifiers from this file; every value goes through sqlStr/sqlNullable.
 */
async function applySyncRecords(table, recs, userId, skipped) {
  if (!recs.length) return;
  const ids = [...new Set(recs.map((r) => r.id))];
  const rows = await query(
    `SELECT id, user_id, updated_at, deleted_at, updated_by_device_id${table === "cloud_lists" ? ", family_id" : ""} FROM ${table} WHERE id IN (${ids.map(sqlStr).join(",")})`,
  );
  const existing = new Map(rows.map((r) => [r.id, r]));
  for (const r of recs) {
    const cols = Object.keys(r.fields);
    const ex = existing.get(r.id);
    if (!ex) {
      await query(
        `INSERT INTO ${table} (id,user_id,${cols.join(",")},created_at,updated_at,updated_by_device_id,deleted_at,version) VALUES (${sqlStr(r.id)},${sqlStr(userId)},${cols.map((c) => sqlStr(r.fields[c])).join(",")},${sqlStr(r.created_at)},${sqlStr(r.updated_at)},${sqlStr(r.updated_by_device_id)},${sqlNullable(r.deleted_at)},1) ON CONFLICT (id) DO NOTHING`,
      );
      existing.set(r.id, { id: r.id, user_id: userId, updated_at: r.updated_at, deleted_at: r.deleted_at, updated_by_device_id: r.updated_by_device_id });
      continue;
    }
    if (ex.user_id !== userId) {
      // Someone else's record: only a list shared into this user's Pocket Family is writable,
      // and only its owner may delete it.
      const familyEdit = table === "cloud_lists" && (await canAccessList(ex, userId));
      if (!familyEdit) {
        skipped.push({ id: r.id, reason: "unavailable" });
        continue;
      }
      if (r.deleted_at) {
        skipped.push({ id: r.id, reason: "not_owner" });
        continue;
      }
    }
    if (!incomingWinsLww(r, ex)) continue; // cloud copy is newer; it comes back in the response
    await query(
      `UPDATE ${table} SET ${cols.map((c) => `${c}=${sqlStr(r.fields[c])}`).join(", ")}, updated_at=${sqlStr(r.updated_at)}, deleted_at=${sqlNullable(r.deleted_at)}, updated_by_device_id=${sqlStr(r.updated_by_device_id)}, version=version+1 WHERE id=${sqlStr(r.id)} AND user_id=${sqlStr(ex.user_id)}`,
    );
    existing.set(r.id, { ...ex, updated_at: r.updated_at, deleted_at: r.deleted_at, updated_by_device_id: r.updated_by_device_id });
  }
}

/** Postgres text timestamps ("2026-11-17 09:30:00+00") → ISO-8601 for the device's parser. */
function syncIsoTimestamps(o) {
  for (const k of ["created_at", "updated_at", "deleted_at"]) {
    if (o[k] == null) continue;
    const t = Date.parse(o[k]);
    if (Number.isFinite(t)) o[k] = new Date(t).toISOString();
  }
  return o;
}

function pgBool(v) {
  return v === true || v === "t" || v === "true";
}

function serializeBookMeta(row) {
  return {
    id: row.id,
    title: row.title || "",
    author: row.author || "",
    format: row.format || "txt",
    filename: row.filename || "",
    size: Number(row.size_bytes || 0),
    size_bytes: Number(row.size_bytes || 0),
    created_at: row.created_at,
  };
}

// ---- Reading progress / Find my Pocket / Home screen order ----
// Mirrors cloud/src/lib/device-features.ts — keep both in step.
const HOME_KEYS = ["today", "notes", "clock", "weather", "music", "reading", "pass", "settings", "update"];
const HOME_ORDER_MAX = 16;
const RING_TTL_MS = 30 * 60 * 1000; // a queued chime is dropped if Pocket doesn't check in by then

function parseReadingProgress(body) {
  const bad = { ok: false, message: "That reading update couldn't be read." };
  if (!body || typeof body !== "object" || Array.isArray(body)) return bad;
  const bookId = typeof body.book_id === "string" ? body.book_id.trim() : "";
  if (!bookId || bookId.length > 128) return bad;
  const { page, pages, percent } = body;
  if (!Number.isInteger(page) || !Number.isInteger(pages) || !Number.isInteger(percent)) return bad;
  if (page < 0 || pages < 0 || page > 1000000 || pages > 1000000) return bad;
  if (percent < 0 || percent > 100) return bad;
  return { ok: true, value: { book_id: bookId, page, pages, percent } };
}

function parseHomeOrder(raw) {
  if (!Array.isArray(raw)) return { ok: false, message: "Home screen order must be a list." };
  if (raw.length > HOME_ORDER_MAX) return { ok: false, message: "Home screen order has too many apps." };
  const seen = new Set();
  for (const k of raw) {
    if (typeof k !== "string" || !HOME_KEYS.includes(k)) {
      return { ok: false, message: "Home screen order has an app Pocket doesn't know." };
    }
    if (seen.has(k)) return { ok: false, message: "Home screen order lists an app twice." };
    seen.add(k);
  }
  return { ok: true, value: raw };
}

function readHomeOrder(json) {
  if (typeof json !== "string" || !json) return [];
  try {
    const parsed = parseHomeOrder(JSON.parse(json));
    return parsed.ok ? parsed.value : [];
  } catch {
    return [];
  }
}

function serializeProgress(p) {
  if (!p) return null;
  const t = Date.parse(p.updated_at);
  return {
    percent: Number(p.percent || 0),
    page: Number(p.page || 0),
    pages: Number(p.pages || 0),
    updated_at: Number.isFinite(t) ? new Date(t).toISOString() : p.updated_at,
  };
}

/** Postgres text timestamp → ISO-8601 (null stays null). */
function isoTs(v) {
  if (v == null || v === "") return null;
  const t = Date.parse(v);
  return Number.isFinite(t) ? new Date(t).toISOString() : v;
}

// ---- Pocket Family (shared lists) ----
// Mirrors cloud/src/lib/family.ts + cloud/src/routes/family.ts — keep both in step.
const FAMILY_MAX_MEMBERS = 6;
const FAMILY_CODE_TTL_MS = 7 * 86400000;
const FAMILY_NAME_MAX = 40;

/** Friendly name shown to other family members: the email's local part. */
function displayName(email) {
  const e = String(email || "");
  const at = e.indexOf("@");
  return (at > 0 ? e.slice(0, at) : e) || "Someone";
}

function parseFamilyName(raw) {
  const name = typeof raw === "string" ? raw.replace(/\s+/g, " ").trim() : "";
  if (!name) return { ok: false, message: "Give your family a name." };
  if (Array.from(name).length > FAMILY_NAME_MAX) {
    return { ok: false, message: `Family name must be ${FAMILY_NAME_MAX} characters or fewer.` };
  }
  return { ok: true, value: name };
}

function generateFamilyCode() {
  const bytes = randomBytes(8);
  let out = "";
  for (let i = 0; i < 8; i++) out += PAIR_ALPHABET[bytes[i] % PAIR_ALPHABET.length];
  return out;
}

/**
 * SQL predicate (alias `l` = cloud_lists) for lists `userId` may read/edit: their own, or lists
 * shared into the family they belong to whose owner is still a member of that family.
 */
function listAccessSql(userId) {
  const u = sqlStr(userId);
  return `(l.user_id=${u} OR (l.family_id IS NOT NULL AND l.family_id=(SELECT fm.family_id FROM family_members fm WHERE fm.user_id=${u}) AND EXISTS (SELECT 1 FROM family_members fo WHERE fo.user_id=l.user_id AND fo.family_id=l.family_id)))`;
}

async function familyIdForUser(userId) {
  const rows = await query(`SELECT family_id FROM family_members WHERE user_id=${sqlStr(userId)}`);
  return rows[0]?.family_id || null;
}

async function canAccessList(row, userId) {
  if (row.user_id === userId) return true;
  if (!row.family_id) return false;
  const mine = await familyIdForUser(userId);
  if (!mine || mine !== row.family_id) return false;
  return (await familyIdForUser(row.user_id)) === row.family_id;
}

async function myFamily(userId) {
  const rows = await query(
    `SELECT f.* FROM families f JOIN family_members m ON m.family_id=f.id WHERE m.user_id=${sqlStr(userId)}`,
  );
  return rows[0] || null;
}

async function serializeFamily(fam, viewerId) {
  const members = await query(
    `SELECT m.user_id, m.role, m.joined_at, u.email FROM family_members m JOIN users u ON u.id=m.user_id WHERE m.family_id=${sqlStr(fam.id)} ORDER BY m.joined_at ASC`,
  );
  const isOwner = fam.owner_user_id === viewerId;
  const expMs = fam.join_code_expires_at ? Date.parse(fam.join_code_expires_at) : NaN;
  const codeLive = !!fam.join_code && Number.isFinite(expMs) && expMs > Date.now();
  return {
    id: fam.id,
    name: fam.name,
    role: isOwner ? "owner" : "member",
    created_at: isoTs(fam.created_at),
    max_members: FAMILY_MAX_MEMBERS,
    // Only the owner invites, so only the owner sees the code.
    join_code: isOwner && codeLive ? fam.join_code : null,
    join_code_expires_at: isOwner && codeLive ? isoTs(fam.join_code_expires_at) : null,
    members: members.map((m) => ({
      user_id: m.user_id,
      name: displayName(m.email),
      role: m.role === "owner" ? "owner" : "member",
      joined_at: isoTs(m.joined_at),
      is_me: m.user_id === viewerId,
    })),
  };
}

async function freshFamilyCode() {
  for (let i = 0; i < 5; i++) {
    const code = generateFamilyCode();
    const clash = await query(`SELECT id FROM families WHERE join_code=${sqlStr(code)}`);
    if (!clash.length) return { code, expires_at: new Date(Date.now() + FAMILY_CODE_TTL_MS).toISOString() };
  }
  return null;
}

/** Companion list shape: sync fields + who owns it + whether it's shared with the family. */
function serializeListFor(row, viewerId) {
  return {
    ...serializeList(row),
    shared: !!row.family_id,
    is_owner: row.user_id === viewerId,
    owner_name: displayName(row.owner_email),
  };
}

async function accessibleList(id, userId) {
  const rows = await query(
    `SELECT l.*, u.email AS owner_email FROM cloud_lists l JOIN users u ON u.id=l.user_id WHERE l.id=${sqlStr(id)} AND ${listAccessSql(userId)}`,
  );
  return rows[0] || null;
}

// ---- Calendar on Today (ICS feeds) ----
// Mirrors cloud/src/lib/calendar.ts + cloud/src/routes/calendar.ts — keep both in step.
const CALENDAR_MAX_FEEDS = 5;
const CALENDAR_LABEL_MAX = 40;
const CALENDAR_URL_MAX = 2048;
const CALENDAR_WINDOW_DAYS = 7;
const CALENDAR_MAX_EVENTS = 30;
const CALENDAR_CACHE_MS = 10 * 60 * 1000;
const CALENDAR_FETCH_TIMEOUT_MS = 8000;
const CALENDAR_MAX_BYTES = 5 * 1024 * 1024;

/** webcal:// → https://; only public https URLs are accepted. */
function normalizeFeedUrl(raw) {
  const bad = { ok: false, message: "Paste the calendar's iCal link (it starts with https:// or webcal://)." };
  let s = typeof raw === "string" ? raw.trim() : "";
  if (!s || s.length > CALENDAR_URL_MAX) return bad;
  if (/^webcals?:\/\//i.test(s)) s = s.replace(/^webcals?:\/\//i, "https://");
  let url;
  try {
    url = new URL(s);
  } catch {
    return bad;
  }
  if (url.protocol !== "https:" || url.username || url.password) return bad;
  const host = url.hostname.toLowerCase().replace(/^\[|\]$/g, "");
  if (
    host === "localhost" || host.endsWith(".localhost") || host.endsWith(".local") || host.endsWith(".internal") ||
    /^\d+\.\d+\.\d+\.\d+$/.test(host) || host.includes(":")
  ) {
    return { ok: false, message: "That calendar link isn't a public web address." };
  }
  return { ok: true, value: url.toString() };
}

function normalizeFeedLabel(raw) {
  const s = typeof raw === "string" ? raw.replace(/\s+/g, " ").trim() : "";
  return Array.from(s || "Calendar").slice(0, CALENDAR_LABEL_MAX).join("");
}

function feedHost(url) {
  try {
    return new URL(url).hostname;
  } catch {
    return "";
  }
}

function serializeFeed(f) {
  return { id: f.id, label: f.label, host: feedHost(f.url), created_at: isoTs(f.created_at) };
}

const calendarCache = new Map(); // url → { at, events } (parsed VEVENTs, expanded per request)

async function loadCalendarFeed(url, now) {
  const hit = calendarCache.get(url);
  if (hit && now - hit.at < CALENDAR_CACHE_MS) return hit.events;
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), CALENDAR_FETCH_TIMEOUT_MS);
  try {
    const res = await globalThis.fetch(url, {
      signal: ctrl.signal,
      redirect: "follow",
      headers: { accept: "text/calendar, */*;q=0.5", "user-agent": "PocketCloud-Calendar/1.0" },
    });
    if (!res.ok) throw new Error(`calendar HTTP ${res.status}`);
    const text = await res.text();
    if (text.length > CALENDAR_MAX_BYTES) throw new Error("calendar too large");
    if (!/BEGIN:VCALENDAR/i.test(text)) throw new Error("not an iCalendar feed");
    const events = ICS.parseIcs(text);
    calendarCache.set(url, { at: now, events });
    if (calendarCache.size > 500) calendarCache.delete(calendarCache.keys().next().value);
    return events;
  } finally {
    clearTimeout(timer);
  }
}

/** Events across a user's feeds overlapping [now, now+7d]. A failing feed is skipped. */
async function upcomingCalendarEvents(feeds, now) {
  const to = new Date(now.getTime() + CALENDAR_WINDOW_DAYS * 86400000);
  const perFeed = await Promise.all(
    feeds.map(async (f) => {
      try {
        return ICS.expandEvents(await loadCalendarFeed(f.url, now.getTime()), now, to, f.label);
      } catch {
        return [];
      }
    }),
  );
  return ICS.mergeCalendarEvents(perFeed, CALENDAR_MAX_EVENTS);
}

// ---- Pocket device backup (settings, alarms, reading places) ----
// Mirrors cloud/src/lib/device-backup.ts + cloud/src/routes/device-backup.ts — keep both in step.
const DEVICE_BACKUP_MAX_BYTES = 64 * 1024;
const DEVICE_BACKUP_KEEP = 5;
const BACKUP_REQUEST_TTL_MS = 24 * 3600 * 1000; // "Back up now" waits this long for a check-in

const isPlainObj = (v) => !!v && typeof v === "object" && !Array.isArray(v);

function parseDeviceBackup(body) {
  const bad = { ok: false, message: "That backup couldn't be read." };
  if (!isPlainObj(body)) return bad;
  const { settings = {}, alarms = [], reading = {} } = body;
  if (!isPlainObj(settings) || !Array.isArray(alarms) || !isPlainObj(reading)) return bad;
  if (body.settings === undefined && body.alarms === undefined && body.reading === undefined) return bad;
  return { ok: true, value: { settings, alarms, reading } };
}

function backupReadingCount(reading) {
  if (Array.isArray(reading.positions)) return reading.positions.length;
  if (Array.isArray(reading.books)) return reading.books.length;
  return Object.keys(reading).length;
}

function readBackupPayload(json) {
  try {
    const parsed = parseDeviceBackup(JSON.parse(json));
    if (parsed.ok) return parsed.value;
  } catch {
    // fall through
  }
  return { settings: {}, alarms: [], reading: {} };
}

// ---- Calendar on Today: iCalendar reader ----
// Generated from cloud/src/lib/ics.ts (esbuild, types stripped) — keep both in step; the TS file
// has the commentary and the unit tests (cloud/tests/ics.test.ts).
const ICS = (() => {
  const DAY_MS = 864e5;
  const WEEKDAYS = ["SU", "MO", "TU", "WE", "TH", "FR", "SA"];
  const MAX_ITERATIONS = 2e4;
  const zoneFormatters = new Map();
  function zoneFormatter(tz) {
    if (zoneFormatters.has(tz)) return zoneFormatters.get(tz);
    let f = null;
    try {
      f = new Intl.DateTimeFormat("en-US", {
        timeZone: tz,
        hourCycle: "h23",
        year: "numeric",
        month: "2-digit",
        day: "2-digit",
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit"
      });
    } catch {
      f = null;
    }
    zoneFormatters.set(tz, f);
    return f;
  }
  function resolveZone(tzid, fallback = "UTC") {
    const raw = String(tzid || "").replace(/^"|"$/g, "").trim();
    if (!raw) return fallback;
    if (zoneFormatter(raw)) return raw;
    const m = raw.match(/([A-Za-z_]+\/[A-Za-z_+-]+(?:\/[A-Za-z_+-]+)?)$/);
    if (m && zoneFormatter(m[1])) return m[1];
    return fallback;
  }
  function zoneOffset(utcMs, tz) {
    if (tz === "UTC") return 0;
    const f = zoneFormatter(tz);
    if (!f) return 0;
    const p = {};
    for (const part of f.formatToParts(new Date(utcMs))) {
      if (part.type !== "literal") p[part.type] = Number(part.value);
    }
    const asUtc = Date.UTC(p.year, p.month - 1, p.day, p.hour % 24, p.minute, p.second);
    return asUtc - Math.floor(utcMs / 1e3) * 1e3;
  }
  function wallToUtc(wall, tz) {
    if (tz === "UTC") return wall;
    const o1 = zoneOffset(wall - 0, tz);
    let t = wall - o1;
    const o2 = zoneOffset(t, tz);
    if (o2 !== o1) t = wall - o2;
    return t;
  }
  function toUtc(t) {
    return t.allDay ? t.wall : wallToUtc(t.wall, t.tz);
  }
  function unfoldLines(text) {
    return text.replace(/\r\n/g, "\n").replace(/\r/g, "\n").replace(/\n[ \t]/g, "").split("\n").filter((l) => l.length > 0);
  }
  function parseLine(line) {
    let i = 0;
    let inQuote = false;
    let colon = -1;
    for (; i < line.length; i++) {
      const ch = line[i];
      if (ch === '"') inQuote = !inQuote;
      else if (ch === ":" && !inQuote) {
        colon = i;
        break;
      }
    }
    if (colon < 0) return null;
    const head = line.slice(0, colon);
    const value = line.slice(colon + 1);
    const parts = head.match(/(?:[^;"]|"[^"]*")+/g) || [];
    const name = (parts[0] || "").toUpperCase();
    const params = {};
    for (const p of parts.slice(1)) {
      const eq = p.indexOf("=");
      if (eq > 0) params[p.slice(0, eq).toUpperCase()] = p.slice(eq + 1).replace(/^"|"$/g, "");
    }
    return { name, params, value };
  }
  function unescapeText(v) {
    return v.replace(/\\([\\;,nN])/g, (_, c) => c === "n" || c === "N" ? "\n" : c);
  }
  function parseIcsTime(value, params, defaultTz = "UTC") {
    const v = value.trim();
    const d = v.match(/^(\d{4})(\d{2})(\d{2})$/);
    if (d || params.VALUE === "DATE") {
      if (!d) return null;
      return { wall: Date.UTC(+d[1], +d[2] - 1, +d[3]), tz: "UTC", allDay: true };
    }
    const m = v.match(/^(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})?(Z)?$/);
    if (!m) return null;
    const wall = Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +(m[6] || 0));
    if (m[7]) return { wall, tz: "UTC", allDay: false };
    return { wall, tz: resolveZone(params.TZID, defaultTz), allDay: false };
  }
  function parseDuration(v) {
    const m = v.trim().match(/^([+-])?P(?:(\d+)W)?(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?)?$/);
    if (!m) return null;
    const ms = (+(m[2] || 0) * 7 + +(m[3] || 0)) * DAY_MS + +(m[4] || 0) * 36e5 + +(m[5] || 0) * 6e4 + +(m[6] || 0) * 1e3;
    return m[1] === "-" ? -ms : ms;
  }
  function parseRule(v, start, defaultTz) {
    const kv = {};
    for (const part of v.split(";")) {
      const eq = part.indexOf("=");
      if (eq > 0) kv[part.slice(0, eq).toUpperCase()] = part.slice(eq + 1);
    }
    const freq = kv.FREQ?.toUpperCase();
    if (freq !== "DAILY" && freq !== "WEEKLY" && freq !== "MONTHLY" && freq !== "YEARLY") return null;
    const interval = Math.max(1, Number.parseInt(kv.INTERVAL || "1", 10) || 1);
    const count = kv.COUNT ? Math.max(0, Number.parseInt(kv.COUNT, 10) || 0) : null;
    let until = null;
    if (kv.UNTIL) {
      const t = parseIcsTime(kv.UNTIL, {}, defaultTz);
      if (t) {
        until = t.allDay ? start.allDay ? t.wall : wallToUtc(t.wall + DAY_MS - 1, start.tz) : toUtc(t);
      }
    }
    const byday = [];
    for (const tok of (kv.BYDAY || "").split(",")) {
      const m = tok.trim().toUpperCase().match(/^([+-]?\d{1,2})?(SU|MO|TU|WE|TH|FR|SA)$/);
      if (m) byday.push({ ord: m[1] ? Number.parseInt(m[1], 10) : 0, day: WEEKDAYS.indexOf(m[2]) });
    }
    return { freq, interval, count, until, byday };
  }
  function parseIcs(text) {
    const lines = unfoldLines(text);
    let defaultTz = "UTC";
    for (const l of lines) {
      if (/^X-WR-TIMEZONE[:;]/i.test(l)) {
        const p = parseLine(l);
        if (p) defaultTz = resolveZone(p.value, "UTC");
        break;
      }
    }
    const out = [];
    let props = null;
    let depth = 0;
    for (const line of lines) {
      const p = parseLine(line);
      if (!p) continue;
      if (p.name === "BEGIN") {
        if (p.value.trim().toUpperCase() === "VEVENT" && !props) {
          props = [];
          depth = 0;
        } else if (props) depth++;
        continue;
      }
      if (p.name === "END") {
        if (!props) continue;
        if (depth > 0) {
          depth--;
          continue;
        }
        if (p.value.trim().toUpperCase() === "VEVENT") {
          const ev = buildEvent(props, defaultTz);
          if (ev) out.push(ev);
          props = null;
        }
        continue;
      }
      if (props && depth === 0) props.push(p);
    }
    return out;
  }
  function buildEvent(props, defaultTz) {
    const first = (n) => props.find((p) => p.name === n);
    const dtstart = first("DTSTART");
    if (!dtstart) return null;
    const start = parseIcsTime(dtstart.value, dtstart.params, defaultTz);
    if (!start) return null;
    let durationMs = null;
    const dtend = first("DTEND");
    const dur = first("DURATION");
    if (dtend) {
      const end = parseIcsTime(dtend.value, dtend.params, defaultTz);
      if (end) durationMs = start.allDay ? end.wall - start.wall : toUtc(end) - toUtc(start);
    } else if (dur) {
      durationMs = parseDuration(dur.value);
    }
    if (durationMs == null && start.allDay) durationMs = DAY_MS;
    if (durationMs != null && durationMs < 0) durationMs = 0;
    const rr = first("RRULE");
    const exdates = [];
    for (const p of props.filter((x) => x.name === "EXDATE")) {
      for (const v of p.value.split(",")) {
        const t = parseIcsTime(v, p.params, defaultTz);
        if (t) exdates.push(t.allDay && !start.allDay ? wallToUtc(t.wall + start.wall % DAY_MS, start.tz) : toUtc(t));
      }
    }
    const rid = first("RECURRENCE-ID");
    const ridTime = rid ? parseIcsTime(rid.value, rid.params, defaultTz) : null;
    return {
      uid: first("UID")?.value.trim() || "",
      summary: unescapeText(first("SUMMARY")?.value || "").trim(),
      location: unescapeText(first("LOCATION")?.value || "").trim(),
      start,
      durationMs,
      rrule: rr ? parseRule(rr.value, start, defaultTz) : null,
      exdates,
      recurrenceId: ridTime ? toUtc(ridTime) : null,
      cancelled: (first("STATUS")?.value || "").trim().toUpperCase() === "CANCELLED"
    };
  }
  function daysInMonth(y, m0) {
    return new Date(Date.UTC(y, m0 + 1, 0)).getUTCDate();
  }
  function nthWeekday(y, m0, weekday, ord) {
    const dim = daysInMonth(y, m0);
    if (ord > 0) {
      const firstDow = new Date(Date.UTC(y, m0, 1)).getUTCDay();
      const day2 = 1 + (weekday - firstDow + 7) % 7 + (ord - 1) * 7;
      return day2 <= dim ? day2 : null;
    }
    const lastDow = new Date(Date.UTC(y, m0, dim)).getUTCDay();
    const day = dim - (lastDow - weekday + 7) % 7 + (ord + 1) * 7;
    return day >= 1 ? day : null;
  }
  function* occurrences(ev, stopWall) {
    const rule = ev.rrule;
    const start = ev.start.wall;
    const tod = (start % DAY_MS + DAY_MS) % DAY_MS;
    const s = new Date(start);
    const sy = s.getUTCFullYear();
    const sm = s.getUTCMonth();
    const sd = s.getUTCDate();
    let emitted = 0;
    let iterations = 0;
    const done = (wall) => {
      if (rule.count != null && emitted >= rule.count) return true;
      if (wall > stopWall) return true;
      if (rule.until != null) {
        const utc = ev.start.allDay ? wall : wallToUtc(wall, ev.start.tz);
        if (utc > rule.until) return true;
      }
      return false;
    };
    let skipPeriods = 0;
    const periodMs = (rule.freq === "DAILY" ? 1 : 7) * DAY_MS * rule.interval;
    if (rule.count == null && (rule.freq === "DAILY" || rule.freq === "WEEKLY")) {
      const lag = stopWall - 40 * DAY_MS - start;
      if (lag > 0) skipPeriods = Math.floor(lag / periodMs);
    }
    for (let k = 0; ; k++) {
      if (++iterations > MAX_ITERATIONS) return;
      const step = k + skipPeriods;
      let candidates = [];
      if (rule.freq === "DAILY") {
        candidates = [start + step * periodMs];
      } else if (rule.freq === "WEEKLY") {
        const days = rule.byday.length ? rule.byday.map((b) => b.day) : [s.getUTCDay()];
        const startDow = (s.getUTCDay() + 6) % 7;
        const weekStart = start - tod - startDow * DAY_MS + step * periodMs;
        candidates = [...new Set(days)].map((d) => weekStart + (d + 6) % 7 * DAY_MS + tod).sort((a, b) => a - b);
      } else if (rule.freq === "MONTHLY") {
        const mi = sm + step * rule.interval;
        const y = sy + Math.floor(mi / 12);
        const m0 = (mi % 12 + 12) % 12;
        if (rule.byday.length) {
          const days = [];
          for (const b of rule.byday) {
            if (b.ord) {
              const d = nthWeekday(y, m0, b.day, b.ord);
              if (d) days.push(d);
            } else {
              for (let d = 1; d <= daysInMonth(y, m0); d++) {
                if (new Date(Date.UTC(y, m0, d)).getUTCDay() === b.day) days.push(d);
              }
            }
          }
          candidates = [...new Set(days)].sort((a, b) => a - b).map((d) => Date.UTC(y, m0, d) + tod);
        } else if (sd <= daysInMonth(y, m0)) {
          candidates = [Date.UTC(y, m0, sd) + tod];
        }
      } else {
        const y = sy + step * rule.interval;
        if (sd <= daysInMonth(y, sm)) candidates = [Date.UTC(y, sm, sd) + tod];
      }
      if (!candidates.length) {
        if (rule.freq === "MONTHLY" || rule.freq === "YEARLY") {
          const probe = rule.freq === "YEARLY" ? Date.UTC(sy + step * rule.interval, 0, 1) : Date.UTC(sy, sm + step * rule.interval, 1);
          if (probe > stopWall) return;
        }
        continue;
      }
      for (const c of candidates) {
        if (c < start) continue;
        if (done(c)) return;
        emitted++;
        yield c;
      }
    }
  }
  function expandEvents(events, from, to, calendar) {
    const fromMs = from.getTime();
    const toMs = to.getTime();
    const SLACK = 14 * 36e5;
    const overrides = new Set();
    for (const ev of events) if (ev.recurrenceId != null) overrides.add(`${ev.uid}|${ev.recurrenceId}`);
    const out = [];
    const emit = (ev, startUtc) => {
      const dur = ev.durationMs;
      const endUtc = dur == null ? null : startUtc + dur;
      const lo = ev.start.allDay ? fromMs - SLACK : fromMs;
      const hi = ev.start.allDay ? toMs + SLACK : toMs;
      const overlaps = endUtc == null || endUtc === startUtc ? startUtc >= lo && startUtc < hi : startUtc < hi && endUtc > lo;
      if (!overlaps) return;
      out.push({
        title: ev.summary || "Busy",
        start: new Date(startUtc).toISOString(),
        end: endUtc == null ? null : new Date(endUtc).toISOString(),
        all_day: ev.start.allDay,
        location: ev.location,
        calendar
      });
    };
    for (const ev of events) {
      if (ev.cancelled) continue;
      if (ev.recurrenceId != null || !ev.rrule) {
        emit(ev, toUtc(ev.start));
        continue;
      }
      const ex = new Set(ev.exdates);
      const stopWall = toMs + 2 * DAY_MS;
      for (const wall of occurrences(ev, stopWall)) {
        const utc = ev.start.allDay ? wall : wallToUtc(wall, ev.start.tz);
        if (ex.has(utc)) continue;
        if (overrides.has(`${ev.uid}|${utc}`)) continue;
        emit(ev, utc);
      }
    }
    return out;
  }
  function mergeCalendarEvents(lists, max = 30) {
    return lists.flat().sort((a, b) => a.start.localeCompare(b.start) || a.title.localeCompare(b.title)).slice(0, max);
  }
  return { parseIcs, expandEvents, mergeCalendarEvents };
})();

function serializeMusicMeta(row) {
  return {
    id: row.id,
    title: row.title || "",
    filename: row.filename || "",
    mime: row.mime || "audio/wav",
    size: Number(row.size_bytes || 0),
    size_bytes: Number(row.size_bytes || 0),
    created_at: row.created_at,
  };
}

// ---- Minimal EPUB (ZIP) reader — no dependency beyond Node's built-in zlib ----
// EPUB is a ZIP of XHTML files. We don't need general ZIP support: just enough to walk the
// central directory, inflate each content document, and join the plain text so Pocket's
// e-ink Reading app can paginate it. Reading order follows the central directory (creation
// order for nearly every EPUB writer); this is a pragmatic v1, not a full OPF spine parser.
function listZipEntries(buf) {
  const EOCD_SIG = 0x06054b50;
  let eocd = -1;
  for (let i = buf.length - 22; i >= 0 && i >= buf.length - 22 - 65557; --i) {
    if (buf.readUInt32LE(i) === EOCD_SIG) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) return [];
  const total = buf.readUInt16LE(eocd + 10);
  let off = buf.readUInt32LE(eocd + 16);
  const entries = [];
  const CD_SIG = 0x02014b50;
  for (let i = 0; i < total; ++i) {
    if (off + 46 > buf.length || buf.readUInt32LE(off) !== CD_SIG) break;
    const compMethod = buf.readUInt16LE(off + 10);
    const compSize = buf.readUInt32LE(off + 20);
    const uncompSize = buf.readUInt32LE(off + 24);
    const nameLen = buf.readUInt16LE(off + 28);
    const extraLen = buf.readUInt16LE(off + 30);
    const commentLen = buf.readUInt16LE(off + 32);
    const localHeaderOffset = buf.readUInt32LE(off + 42);
    const name = buf.toString("utf8", off + 46, off + 46 + nameLen);
    entries.push({ name, compMethod, compSize, uncompSize, localHeaderOffset });
    off += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

function readZipEntry(buf, entry) {
  const LFH_SIG = 0x04034b50;
  const off = entry.localHeaderOffset;
  if (off + 30 > buf.length || buf.readUInt32LE(off) !== LFH_SIG) return null;
  const nameLen = buf.readUInt16LE(off + 26);
  const extraLen = buf.readUInt16LE(off + 28);
  const dataStart = off + 30 + nameLen + extraLen;
  const raw = buf.subarray(dataStart, dataStart + entry.compSize);
  if (entry.compMethod === 0) return Buffer.from(raw);
  if (entry.compMethod === 8) {
    try {
      return inflateRawSync(raw);
    } catch {
      return null;
    }
  }
  return null;
}

function htmlToPlainText(html) {
  let s = String(html);
  s = s.replace(/<(script|style)[^>]*>[\s\S]*?<\/\1>/gi, " ");
  s = s.replace(/<(p|div|br|h[1-6]|li|tr)[^>]*>/gi, "\n");
  s = s.replace(/<\/(p|div|h[1-6]|li|tr)>/gi, "\n");
  s = s.replace(/<[^>]+>/g, " ");
  s = s
    .replace(/&nbsp;/gi, " ")
    .replace(/&amp;/gi, "&")
    .replace(/&lt;/gi, "<")
    .replace(/&gt;/gi, ">")
    .replace(/&#(\d+);/g, (_, d) => String.fromCharCode(Number(d)))
    .replace(/&quot;/gi, '"')
    .replace(/&#39;/gi, "'");
  s = s.replace(/[ \t]+/g, " ").replace(/\n[ \t]+/g, "\n").replace(/\n{3,}/g, "\n\n");
  return s.trim();
}

function extractEpubText(buf) {
  const entries = listZipEntries(buf);
  const content = entries.filter((e) => /\.(x?html?|xhtml)$/i.test(e.name));
  const skip = /nav\.x?html?$|toc\.x?html?$|cover\.x?html?$/i;
  const ordered = content.filter((e) => !skip.test(e.name)).length ? content.filter((e) => !skip.test(e.name)) : content;
  const parts = [];
  for (const entry of ordered) {
    const data = readZipEntry(buf, entry);
    if (!data) continue;
    const text = htmlToPlainText(data.toString("utf8"));
    if (text) parts.push(text);
  }
  return parts.join("\n\n").trim();
}

const MUSIC_MAX_INTERNAL_BYTES = 2 * 1024 * 1024; // LittleFS-safe, no SD
const MUSIC_MAX_SD_BYTES = 80 * 1024 * 1024; // when Pocket reports a microSD card
const MUSIC_MAX_BYTES = MUSIC_MAX_SD_BYTES; // cloud accepts up to the SD ceiling

const BOOK_MAX_BYTES = 20 * 1024 * 1024; // original upload (epub/txt) — text extracted server-side
const BOOK_MAX_TEXT_BYTES = 4 * 1024 * 1024; // normalized text stored/synced to the device

/**
 * Device auth: the shared build key plus, once this Pocket has been issued one, its own
 * x-device-token (pocket-contract v1.0). Links whose device never collected a token
 * (older firmware) keep working on the key alone until it does. `strict` (wallpapers,
 * apps) always requires the token.
 */
async function deviceLinkForRequest(req, deviceId, { strict = false } = {}) {
  const key = (req.headers.get("x-device-key") || "").trim();
  if (!safeEqual(key, DEVICE_API_KEY)) return null;
  const id = String(deviceId || "").trim();
  if (!id) return null;
  const rows = await query(
    `SELECT id, user_id, device_token_hash, token_issued_at FROM device_links WHERE device_id='${esc(id)}' ORDER BY linked_at DESC LIMIT 1`,
  );
  const link = rows[0];
  if (!link) return null;
  const tok = (req.headers.get("x-device-token") || "").trim();
  if (tok) {
    if (!safeEqual(sha256Hex(tok), String(link.device_token_hash || ""))) return null;
  } else if (strict || link.token_issued_at) {
    return null;
  }
  return link;
}

async function userIdForDeviceKey(req, deviceId) {
  const link = await deviceLinkForRequest(req, deviceId);
  return link?.user_id || null;
}

// ---- Wallpapers (pocket-contract v1.0 §B) -------------------------------------------
const WALL_W = 480, WALL_H = 800, WALL_BYTES = (WALL_W / 8) * WALL_H; // 48,000
const WALL_HEADER = Buffer.from(`P4\n${WALL_W} ${WALL_H}\n`);
const WALL_MAX_PER_DEVICE = 5;

/** Binary PBM (P4) → canonical 480×800 bytes (header + 48,000), or null. */
function parseWallPbm(b64) {
  if (typeof b64 !== "string" || b64.length > 70000) return null;
  const buf = Buffer.from(b64.replace(/^data:[^,]*,/, ""), "base64");
  // P4 <ws> width <ws> height <single ws> raster
  const head = buf.subarray(0, 32).toString("latin1");
  const m = head.match(/^P4\s+(\d+)\s+(\d+)\s/);
  if (!m || Number(m[1]) !== WALL_W || Number(m[2]) !== WALL_H) return null;
  const raster = buf.subarray(m[0].length);
  if (raster.length !== WALL_BYTES) return null;
  return Buffer.concat([WALL_HEADER, raster]);
}

function parseWallLayout(layout) {
  const out = { clock: null, info: { show: false, ink: "black" } };
  if (layout == null) return out;
  if (typeof layout !== "object" || Array.isArray(layout)) return null;
  const inkOk = (v) => v === undefined || v === "black" || v === "white";
  if (layout.clock != null) {
    const c = layout.clock;
    const n = (v) => Number.isInteger(v);
    if (typeof c !== "object" || !n(c.x) || !n(c.y) || !n(c.w) || !n(c.h)) return null;
    if (c.x < 0 || c.y < 0 || c.w < 120 || c.h < 48 || c.x + c.w > WALL_W || c.y + c.h > WALL_H) return null;
    if (!(c.style === undefined || c.style === "large" || c.style === "small") || !inkOk(c.ink)) return null;
    out.clock = { x: c.x, y: c.y, w: c.w, h: c.h, style: c.style || "large", ink: c.ink || "black" };
  }
  if (layout.info != null) {
    const i = layout.info;
    if (typeof i !== "object" || (i.show !== undefined && typeof i.show !== "boolean") || !inkOk(i.ink)) return null;
    out.info = { show: i.show === true, ink: i.ink || "black" };
  }
  return out;
}

function wallDevices(row) {
  try {
    const d = JSON.parse(row.devices_json || '"all"');
    return d === "all" ? "all" : Array.isArray(d) ? d.map(String) : "all";
  } catch {
    return "all";
  }
}

function wallLayout(row) {
  try {
    return JSON.parse(row.layout_json || "{}");
  } catch {
    return { clock: null, info: { show: false, ink: "black" } };
  }
}

function serializeWallpaper(req, row) {
  const origin = new URL(req.url).origin;
  return {
    id: row.id,
    name: row.name,
    sha256: row.sha256,
    created_at: row.created_at,
    devices: wallDevices(row),
    layout: wallLayout(row),
    preview_url: `${origin}/v1/wallpapers/${row.id}/image.pbm`,
  };
}

async function wallpapersForUser(userId) {
  return query(
    `SELECT id, name, sha256, layout_json, devices_json, created_at FROM wallpapers WHERE user_id='${esc(userId)}' ORDER BY created_at DESC`,
  );
}

function wallTargets(row, deviceId) {
  const d = wallDevices(row);
  return d === "all" || d.includes(deviceId);
}

/** Which of the user's devices a "devices" value names; null if it names one they don't own. */
async function resolveWallDevices(userId, devices) {
  if (devices === "all" || devices === undefined) return "all";
  if (!Array.isArray(devices) || devices.length === 0 || devices.length > 20) return null;
  const owned = await query(`SELECT device_id FROM device_links WHERE user_id='${esc(userId)}'`);
  const set = new Set(owned.map((r) => r.device_id));
  const ids = [...new Set(devices.map(String))];
  return ids.every((d) => set.has(d)) ? ids : null;
}

// ---- Apps (pocket-contract v1.0 §C–E) -----------------------------------------------
const APP_MAX_PER_DEVICE = 6; // Home has six free tiles
const FEED_MAX_BYTES = 16 * 1024;
const REGISTRY_URL = (process.env.POCKET_REGISTRY_URL || "https://northwork.dev/api/pocket/registry").trim();
const REGISTRY_EVERY_MS = 10 * 60e3;
const APP_RETURN_HOSTS = new Set(["northwork.app", "www.northwork.app", "northwork.dev", "www.northwork.dev"]);
const APP_DEFAULT_RETURN = "https://northwork.app/pocket/app/apps";
const OAUTH_PROVIDERS = {
  guide: {
    issuer: "https://guidelearning.org",
    clientIdEnv: "GUIDE_CLIENT_ID",
    secretEnv: "GUIDE_CLIENT_SECRET",
    redirect: "https://northwork.app/pocket/app/oauth/guide/callback",
    tokenAuth: "basic", // Guide: client_id + guide:access key as HTTP Basic
  },
};
const oauthDiscovery = new Map();

const strOk = (v, max, min = 1) => typeof v === "string" && v.length >= min && v.length <= max;
/** Plain text for the 1-bit fonts: no control chars, no astral-plane emoji. */
const cleanText = (v) => String(v).replace(/[\u0000-\u0008\u000b-\u001f\u007f]/g, "").replace(/[\u{10000}-\u{10FFFF}]/gu, "").trim();

function iconOk(b64) {
  if (typeof b64 !== "string" || b64.length > 400) return false;
  const buf = Buffer.from(b64, "base64");
  const m = buf.subarray(0, 16).toString("latin1").match(/^P4\s+32\s+32\s/);
  return Boolean(m) && buf.length - m[0].length === 128;
}

/** pocket-app/1 manifest → normalized, or null when invalid. */
function parseAppManifest(m) {
  if (!m || typeof m !== "object" || m.schema !== "pocket-app/1") return null;
  if (typeof m.id !== "string" || !/^[a-z0-9](?:[a-z0-9.-]{0,62}[a-z0-9])?$/.test(m.id)) return null;
  if (typeof m.version !== "string" || !/^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$/.test(m.version)) return null;
  if (!strOk(m.name, 20) || (m.summary !== undefined && !strOk(m.summary, 80, 0))) return null;
  if (!m.developer || !strOk(m.developer.id, 64) || !strOk(m.developer.name, 64)) return null;
  if (!iconOk(m.icon_pbm_b64)) return null;
  let dataUrl;
  try { dataUrl = new URL(m.data_url); } catch { return null; }
  if (dataUrl.protocol !== "https:") return null;
  const refresh = Number(m.refresh_minutes ?? 30);
  if (!Number.isInteger(refresh) || refresh < 15 || refresh > 1440) return null;
  let auth = { type: "none" };
  if (m.auth && m.auth.type === "oauth2") {
    if (!OAUTH_PROVIDERS[m.auth.provider]) return null;
    const scopes = Array.isArray(m.auth.scopes) ? m.auth.scopes.filter((x) => typeof x === "string" && /^[\w.:-]{1,64}$/.test(x)) : [];
    auth = { type: "oauth2", provider: m.auth.provider, scopes: scopes.slice(0, 20) };
  } else if (m.auth && m.auth.type !== "none") {
    return null;
  }
  const surfaces = (Array.isArray(m.surfaces) ? m.surfaces : ["app"]).filter((x) => x === "app" || x === "today");
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

/** pocket-feed/1 → normalized feed, or null (the last good feed is kept). */
function parseAppFeed(f) {
  if (!f || typeof f !== "object" || f.schema !== "pocket-feed/1") return null;
  if (!strOk(f.title, 40)) return null;
  if (f.today !== undefined && f.today !== null && !strOk(f.today, 40, 0)) return null;
  if (!Array.isArray(f.screens) || f.screens.length < 1 || f.screens.length > 8) return null;
  const ids = new Set();
  for (const sc of f.screens) {
    if (!sc || typeof sc.id !== "string" || !/^[A-Za-z0-9_-]{1,32}$/.test(sc.id) || ids.has(sc.id)) return null;
    ids.add(sc.id);
  }
  const screens = [];
  for (const sc of f.screens) {
    if (!strOk(sc.title, 40) || !Array.isArray(sc.blocks) || sc.blocks.length > 40) return null;
    const blocks = [];
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
          const row = { type: "row", text: cleanText(b.text) };
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

function appReturnUrl(v) {
  try {
    const u = new URL(String(v || ""));
    if (u.protocol === "https:" && APP_RETURN_HOSTS.has(u.hostname)) return u.toString();
  } catch {}
  return APP_DEFAULT_RETURN;
}

/** Pull approved manifests from northwork.dev at most every 10 minutes (signed). */
async function maybePollRegistry(force = false) {
  const secret = (process.env.POCKET_REGISTRY_SECRET || "").trim();
  if (!secret) return;
  const last = Number(await getAppSetting("pocket_registry_polled_at")) || 0;
  if (!force && Date.now() - last < REGISTRY_EVERY_MS) return;
  await setAppSetting("pocket_registry_polled_at", String(Date.now()), "registry");
  let raw = "";
  let sig = "";
  try {
    const res = await globalThis.fetch(REGISTRY_URL, {
      headers: { authorization: `Bearer ${secret}`, accept: "application/json", "user-agent": "Pocket-Cloud/1.0" },
      signal: AbortSignal.timeout(8000),
    });
    if (!res.ok) { console.warn("registry HTTP", res.status); return; }
    raw = await res.text();
    sig = (res.headers.get("x-pocket-signature") || "").trim().toLowerCase();
  } catch (e) {
    console.warn("registry fetch failed", String(e && e.message || e));
    return;
  }
  const expect = createHmac("sha256", secret).update(raw).digest("hex");
  if (!sig || !safeEqual(sig, expect)) { console.warn("registry signature mismatch"); return; }
  let body;
  try { body = JSON.parse(raw); } catch { return; }
  const apps = Array.isArray(body.apps) ? body.apps : [];
  const revoked = new Set((Array.isArray(body.revoked) ? body.revoked : []).map(String));
  const now = new Date().toISOString();
  const keep = new Set();
  for (const m of apps) {
    const app = parseAppManifest(m);
    if (!app || revoked.has(app.id) || revoked.has(`${app.id}@${app.version}`)) continue;
    keep.add(app.id);
    await query(`DELETE FROM app_registry WHERE id='${esc(app.id)}'`);
    await query(`INSERT INTO app_registry (id,version,manifest_json,updated_at) VALUES ('${esc(app.id)}','${esc(app.version)}',${sqlStr(JSON.stringify(app))},'${now}')`);
  }
  // Anything no longer approved (or revoked) disappears from Pockets on their next check-in.
  const current = await query(`SELECT id FROM app_registry`);
  for (const r of current) {
    if (!keep.has(r.id)) await query(`DELETE FROM app_registry WHERE id='${esc(r.id)}'`);
  }
}

async function registryApps() {
  const rows = await query(`SELECT id, manifest_json FROM app_registry ORDER BY id`);
  const out = new Map();
  for (const r of rows) {
    try { out.set(r.id, JSON.parse(r.manifest_json)); } catch {}
  }
  return out;
}

async function appInstalls(userId) {
  return query(`SELECT app_id, devices_json, installed_at FROM app_installs WHERE user_id='${esc(userId)}' ORDER BY installed_at`);
}

async function appAuthRow(userId, provider) {
  const rows = await query(`SELECT access_token, refresh_token, expires_at, status, updated_at FROM app_auth WHERE user_id='${esc(userId)}' AND provider='${esc(provider)}'`);
  return rows[0] || null;
}

async function oauthConfig(provider) {
  const p = OAUTH_PROVIDERS[provider];
  if (!p) return null;
  const clientId = (process.env[p.clientIdEnv] || "").trim();
  const secret = (process.env[p.secretEnv] || "").trim();
  if (!clientId || !secret) return null;
  let d = oauthDiscovery.get(provider);
  if (!d || Date.now() - d.at > 3600e3) {
    try {
      const res = await globalThis.fetch(`${p.issuer}/.well-known/openid-configuration`, { signal: AbortSignal.timeout(8000) });
      if (!res.ok) return null;
      const j = await res.json();
      if (!j.authorization_endpoint || !j.token_endpoint) return null;
      d = { at: Date.now(), doc: j };
      oauthDiscovery.set(provider, d);
    } catch {
      return null;
    }
  }
  return { ...p, clientId, secret, doc: d.doc };
}

/** Start "Log in with <provider>": a one-time state bound to this user, with PKCE. */
async function appAuthUrl(userId, app, returnUrl) {
  const cfg = await oauthConfig(app.auth.provider);
  if (!cfg) return null;
  const state = randomToken(24);
  const verifier = randomToken(48);
  const challenge = createHash("sha256").update(verifier).digest("base64url");
  await query(`DELETE FROM app_oauth_states WHERE created_at < '${new Date(Date.now() - 3600e3).toISOString()}'`);
  await query(`INSERT INTO app_oauth_states (state,user_id,provider,app_id,return_url,verifier,created_at) VALUES ('${esc(state)}','${esc(userId)}','${esc(app.auth.provider)}','${esc(app.id)}',${sqlStr(returnUrl)},'${esc(verifier)}','${new Date().toISOString()}')`);
  const u = new URL(cfg.doc.authorization_endpoint);
  u.searchParams.set("response_type", "code");
  u.searchParams.set("client_id", cfg.clientId);
  u.searchParams.set("redirect_uri", cfg.redirect);
  u.searchParams.set("scope", (app.auth.scopes.length ? app.auth.scopes : ["openid", "offline_access"]).join(" "));
  u.searchParams.set("state", state);
  u.searchParams.set("code_challenge", challenge);
  u.searchParams.set("code_challenge_method", "S256");
  return u.toString();
}

async function oauthTokenRequest(cfg, params) {
  const methods = Array.isArray(cfg.doc.token_endpoint_auth_methods_supported) ? cfg.doc.token_endpoint_auth_methods_supported : [];
  const headers = { "content-type": "application/x-www-form-urlencoded", accept: "application/json" };
  const form = new URLSearchParams(params);
  if (cfg.tokenAuth === "basic" || (methods.includes("client_secret_basic") && !methods.includes("client_secret_post"))) {
    headers.authorization = "Basic " + Buffer.from(`${encodeURIComponent(cfg.clientId)}:${encodeURIComponent(cfg.secret)}`).toString("base64");
  } else {
    form.set("client_id", cfg.clientId);
    form.set("client_secret", cfg.secret);
  }
  const res = await globalThis.fetch(cfg.doc.token_endpoint, { method: "POST", headers, body: form.toString(), signal: AbortSignal.timeout(8000) });
  const j = await res.json().catch(() => ({}));
  return { ok: res.ok, status: res.status, body: j };
}

async function storeAppTokens(userId, provider, tok) {
  const expires = new Date(Date.now() + Math.max(60, Number(tok.expires_in) || 3600) * 1000).toISOString();
  const now = new Date().toISOString();
  const prev = await appAuthRow(userId, provider);
  const refresh = tok.refresh_token || prev?.refresh_token || null;
  await query(`DELETE FROM app_auth WHERE user_id='${esc(userId)}' AND provider='${esc(provider)}'`);
  await query(`INSERT INTO app_auth (user_id,provider,access_token,refresh_token,expires_at,status,updated_at) VALUES ('${esc(userId)}','${esc(provider)}',${sqlStr(String(tok.access_token))},${sqlNullable(refresh)},'${expires}','ok','${now}')`);
}

async function setAppAuthStatus(userId, provider, status) {
  await query(`UPDATE app_auth SET status='${esc(status)}', updated_at='${new Date().toISOString()}' WHERE user_id='${esc(userId)}' AND provider='${esc(provider)}'`);
}

/**
 * A usable access token, or null. Refresh tokens rotate and a spent one ends the whole
 * sign-in, so: one refresh at a time per user (status 'refreshing' claimed atomically), the
 * new refresh token is stored before the access token is used, and a refresh whose outcome
 * is unknown (timeout, dropped connection) is never retried — the user reconnects instead.
 */
async function appAccessToken(userId, provider, { force = false } = {}) {
  const row = await appAuthRow(userId, provider);
  if (!row || row.status !== "ok") return null;
  if (!force && Date.parse(row.expires_at) - Date.now() > 60e3) return row.access_token;
  if (!row.refresh_token) {
    await setAppAuthStatus(userId, provider, "reconnect");
    return null;
  }
  const cfg = await oauthConfig(provider);
  if (!cfg) return null;
  const claimed = await query(`UPDATE app_auth SET status='refreshing', updated_at='${new Date().toISOString()}' WHERE user_id='${esc(userId)}' AND provider='${esc(provider)}' AND status='ok' AND refresh_token='${esc(row.refresh_token)}' RETURNING refresh_token`);
  if (!claimed[0]) return null; // another request is refreshing (or already did)
  let r;
  try {
    r = await oauthTokenRequest(cfg, { grant_type: "refresh_token", refresh_token: row.refresh_token });
  } catch {
    await setAppAuthStatus(userId, provider, "reconnect"); // outcome unknown: never reuse that token
    return null;
  }
  if (r.ok && r.body.access_token) {
    await storeAppTokens(userId, provider, r.body);
    return r.body.access_token;
  }
  // invalid_grant (or any 4xx): the sign-in is over. 5xx: the server answered without
  // rotating, so the token stays usable for a later try.
  await setAppAuthStatus(userId, provider, r.status >= 500 ? "ok" : "reconnect");
  return null;
}

/** Opaque, stable per-developer user id (never the email). */
function appUserId(userId, app) {
  const key = (process.env.SESSION_SECRET || DEVICE_API_KEY || "pocket").trim();
  return createHmac("sha256", key).update(`${userId}:${app.developer.id}`).digest("hex").slice(0, 32);
}

async function fetchAppFeed(app, userId, accessToken) {
  const headers = { accept: "application/json", "x-pocket-user": appUserId(userId, app), "user-agent": "Pocket-Cloud/1.0" };
  if (accessToken) headers.authorization = `Bearer ${accessToken}`;
  try {
    const res = await globalThis.fetch(app.data_url, { headers, redirect: "error", signal: AbortSignal.timeout(8000) });
    if (res.status === 401) return { error: "HTTP 401", unauthorized: true };
    if (!res.ok) return { error: `HTTP ${res.status}` };
    const len = Number(res.headers.get("content-length") || 0);
    if (len > FEED_MAX_BYTES) return { error: "too large" };
    const raw = await res.text();
    if (Buffer.byteLength(raw) > FEED_MAX_BYTES) return { error: "too large" };
    const feed = parseAppFeed(JSON.parse(raw));
    return feed ? { feed } : { error: "invalid feed" };
  } catch (e) {
    return { error: String((e && e.message) || e).slice(0, 120) };
  }
}

/** Installed apps for one Pocket, with feeds refreshed when stale. */
async function deviceAppsPayload(userId, deviceId, { refresh = true } = {}) {
  const reg = await registryApps();
  const installs = (await appInstalls(userId)).filter((r) => wallTargets(r, deviceId) && reg.has(r.app_id)).slice(0, APP_MAX_PER_DEVICE);
  const feeds = new Map((await query(`SELECT app_id, feed_json, fetched_at, error FROM app_feeds WHERE user_id='${esc(userId)}'`)).map((r) => [r.app_id, r]));
  const out = [];
  for (const inst of installs) {
    const app = reg.get(inst.app_id);
    let status = "ok";
    let token = null;
    if (app.auth.type === "oauth2") {
      const a = await appAuthRow(userId, app.auth.provider);
      if (!a) status = "connect";
      else if (a.status === "refreshing") status = Date.now() - Date.parse(a.updated_at) > 5 * 60e3 ? "reconnect" : "waiting";
      else if (a.status !== "ok") status = "reconnect";
    }
    const cached = feeds.get(app.id);
    const stale = !cached || Date.now() - Date.parse(cached.fetched_at) > app.refresh_minutes * 60e3;
    if (refresh && stale && status === "ok") {
      if (app.auth.type === "oauth2") {
        token = await appAccessToken(userId, app.auth.provider);
        if (!token) {
          const a = await appAuthRow(userId, app.auth.provider);
          status = a && a.status !== "ok" ? "reconnect" : "waiting";
        }
      }
      if (status === "ok") {
        let r = await fetchAppFeed(app, userId, token);
        if (r.unauthorized && app.auth.type === "oauth2") {
          token = await appAccessToken(userId, app.auth.provider, { force: true });
          if (token) r = await fetchAppFeed(app, userId, token);
          else {
            const a = await appAuthRow(userId, app.auth.provider);
            status = a && a.status === "reconnect" ? "reconnect" : "waiting";
          }
        }
        const now = new Date().toISOString();
        const feedJson = r.feed ? JSON.stringify(r.feed) : cached?.feed_json || null;
        await query(`DELETE FROM app_feeds WHERE user_id='${esc(userId)}' AND app_id='${esc(app.id)}'`);
        await query(`INSERT INTO app_feeds (user_id,app_id,feed_json,fetched_at,error) VALUES ('${esc(userId)}','${esc(app.id)}',${sqlNullable(feedJson)},'${now}',${sqlNullable(r.error || null)})`);
        feeds.set(app.id, { app_id: app.id, feed_json: feedJson, fetched_at: now, error: r.error || null });
      }
    }
    const f = feeds.get(app.id);
    let feed = null;
    try { feed = f && f.feed_json ? JSON.parse(f.feed_json) : null; } catch {}
    if (status === "ok" && !feed) status = "waiting";
    out.push({
      id: app.id,
      name: app.name,
      version: app.version,
      icon_pbm_b64: app.icon_pbm_b64,
      surfaces: app.surfaces,
      status,
      feed,
      updated_at: f?.fetched_at || null,
    });
  }
  return out;
}

function appsRevFor(rows, deviceId, reg) {
  const parts = rows.filter((r) => wallTargets(r, deviceId) && reg.has(r.app_id)).map((r) => `${r.app_id}@${reg.get(r.app_id).version}`);
  return createHash("sha256").update(parts.join("|")).digest("hex").slice(0, 16);
}

/** Devices that would end up with more than 5 wallpapers. */
async function wallOverLimit(userId, rows, devices, exceptId) {
  const owned = (await query(`SELECT device_id FROM device_links WHERE user_id='${esc(userId)}'`)).map((r) => r.device_id);
  const targets = devices === "all" ? owned : devices;
  const others = rows.filter((r) => r.id !== exceptId);
  if (!targets.length) return others.length >= WALL_MAX_PER_DEVICE ? ["all"] : [];
  return targets.filter((d) => others.filter((r) => wallTargets(r, d)).length >= WALL_MAX_PER_DEVICE);
}

async function audit(actorId, action, targetType, targetId, detail) {
  const now = new Date().toISOString();
  const detailText =
    detail == null ? null : typeof detail === "string" ? detail : JSON.stringify(detail);
  await query(
    `INSERT INTO admin_audit_log (id,actor_user_id,action,target_type,target_id,detail,created_at) VALUES ('${esc(newId())}','${esc(actorId || "")}','${esc(action)}','${esc(targetType || "")}','${esc(targetId || "")}',${sqlNullable(detailText)},'${now}')`,
  );
}

async function ensureAdminSchema() {
  await query(`ALTER TABLE users ADD COLUMN IF NOT EXISTS role TEXT NOT NULL DEFAULT 'user'`);
  await query(`ALTER TABLE users ADD COLUMN IF NOT EXISTS disabled_at TIMESTAMPTZ`);
  await query(`ALTER TABLE users ADD COLUMN IF NOT EXISTS last_login_at TIMESTAMPTZ`);
  await query(`ALTER TABLE device_links ADD COLUMN IF NOT EXISTS sd_present BOOLEAN NOT NULL DEFAULT false`);
  await query(`ALTER TABLE device_links ADD COLUMN IF NOT EXISTS pending_wifi_ssid TEXT`);
  await query(`ALTER TABLE device_links ADD COLUMN IF NOT EXISTS pending_wifi_password TEXT`);
  await query(`ALTER TABLE device_links ADD COLUMN IF NOT EXISTS pending_wifi_at TIMESTAMPTZ`);
  await query(`ALTER TABLE device_links ADD COLUMN IF NOT EXISTS parental_json TEXT NOT NULL DEFAULT '{}'`);
  await query(`ALTER TABLE device_links ADD COLUMN IF NOT EXISTS lock_message TEXT NOT NULL DEFAULT ''`);
  await query(`ALTER TABLE device_links ADD COLUMN IF NOT EXISTS volume_percent INTEGER NOT NULL DEFAULT 80`);
  await query(`ALTER TABLE device_links ADD COLUMN IF NOT EXISTS brightness_percent INTEGER NOT NULL DEFAULT 50`);
  await query(`ALTER TABLE device_links ADD COLUMN IF NOT EXISTS ring_requested_at TIMESTAMPTZ`);
  await query(`ALTER TABLE device_links ADD COLUMN IF NOT EXISTS home_order_json TEXT NOT NULL DEFAULT '[]'`);
  // pocket-contract v1.0: per-device token. token_issued_at = when the Pocket itself collected it.
  await query(`ALTER TABLE device_links ADD COLUMN IF NOT EXISTS token_issued_at TIMESTAMPTZ`);
  await query(`ALTER TABLE pair_sessions ADD COLUMN IF NOT EXISTS device_token_once TEXT`);
  await query(`CREATE TABLE IF NOT EXISTS wallpapers (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    image_b64 TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    layout_json TEXT NOT NULL DEFAULT '{}',
    devices_json TEXT NOT NULL DEFAULT '"all"',
    created_at TIMESTAMPTZ NOT NULL
  )`);
  await query(`CREATE INDEX IF NOT EXISTS wallpapers_user ON wallpapers(user_id, created_at DESC)`);
  // Apps (pocket-contract v1.0 §C–E)
  await query(`CREATE TABLE IF NOT EXISTS app_registry (id TEXT PRIMARY KEY, version TEXT NOT NULL, manifest_json TEXT NOT NULL, updated_at TIMESTAMPTZ NOT NULL)`);
  await query(`CREATE TABLE IF NOT EXISTS app_installs (user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, app_id TEXT NOT NULL, devices_json TEXT NOT NULL DEFAULT '"all"', installed_at TIMESTAMPTZ NOT NULL, PRIMARY KEY (user_id, app_id))`);
  await query(`CREATE TABLE IF NOT EXISTS app_auth (user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, provider TEXT NOT NULL, access_token TEXT NOT NULL, refresh_token TEXT, expires_at TIMESTAMPTZ NOT NULL, status TEXT NOT NULL DEFAULT 'ok', updated_at TIMESTAMPTZ NOT NULL, PRIMARY KEY (user_id, provider))`);
  await query(`CREATE TABLE IF NOT EXISTS app_feeds (user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, app_id TEXT NOT NULL, feed_json TEXT, fetched_at TIMESTAMPTZ NOT NULL, error TEXT, PRIMARY KEY (user_id, app_id))`);
  await query(`CREATE TABLE IF NOT EXISTS app_oauth_states (state TEXT PRIMARY KEY, user_id TEXT NOT NULL, provider TEXT NOT NULL, app_id TEXT NOT NULL, return_url TEXT NOT NULL, verifier TEXT NOT NULL, created_at TIMESTAMPTZ NOT NULL)`);
  await query(`CREATE TABLE IF NOT EXISTS reading_progress (
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    book_id TEXT NOT NULL,
    device_id TEXT NOT NULL DEFAULT '',
    page INTEGER NOT NULL DEFAULT 0,
    pages INTEGER NOT NULL DEFAULT 0,
    percent INTEGER NOT NULL DEFAULT 0,
    updated_at TIMESTAMPTZ NOT NULL,
    PRIMARY KEY (user_id, book_id)
  )`);
  await query(`CREATE TABLE IF NOT EXISTS badges (
    id TEXT PRIMARY KEY,
    key TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    icon_key TEXT NOT NULL DEFAULT 'star',
    description TEXT NOT NULL DEFAULT '',
    created_at TIMESTAMPTZ NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL
  )`);
  await query(`CREATE TABLE IF NOT EXISTS badge_awards (
    id TEXT PRIMARY KEY,
    badge_id TEXT NOT NULL REFERENCES badges(id) ON DELETE CASCADE,
    user_id TEXT REFERENCES users(id) ON DELETE CASCADE,
    device_link_id TEXT REFERENCES device_links(id) ON DELETE CASCADE,
    awarded_by TEXT,
    note TEXT,
    awarded_at TIMESTAMPTZ NOT NULL
  )`);
  await query(`CREATE TABLE IF NOT EXISTS admin_audit_log (
    id TEXT PRIMARY KEY,
    actor_user_id TEXT,
    action TEXT NOT NULL,
    target_type TEXT,
    target_id TEXT,
    detail TEXT,
    created_at TIMESTAMPTZ NOT NULL
  )`);
  await query(`CREATE TABLE IF NOT EXISTS app_settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL DEFAULT '',
    updated_at TIMESTAMPTZ NOT NULL,
    updated_by TEXT
  )`);
  await query(`CREATE TABLE IF NOT EXISTS stripe_events (
    id TEXT PRIMARY KEY,
    received_at TIMESTAMPTZ NOT NULL
  )`);
  await query(`CREATE TABLE IF NOT EXISTS mock_checkouts (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL,
    with_trial INT NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL,
    completed_at TIMESTAMPTZ
  )`);
  await query(`CREATE TABLE IF NOT EXISTS cloud_notes (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title TEXT NOT NULL DEFAULT '',
    body TEXT NOT NULL DEFAULT '',
    created_at TIMESTAMPTZ NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL,
    updated_by_device_id TEXT NOT NULL,
    deleted_at TIMESTAMPTZ,
    version INTEGER NOT NULL DEFAULT 1
  )`);
  await query(`CREATE TABLE IF NOT EXISTS cloud_lists (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title TEXT NOT NULL DEFAULT '',
    items_json TEXT NOT NULL DEFAULT '[]',
    created_at TIMESTAMPTZ NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL,
    updated_by_device_id TEXT NOT NULL,
    deleted_at TIMESTAMPTZ,
    version INTEGER NOT NULL DEFAULT 1
  )`);
  await query(`CREATE TABLE IF NOT EXISTS backups (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL,
    blob_json TEXT NOT NULL,
    note_count INTEGER NOT NULL,
    list_count INTEGER NOT NULL,
    size_bytes INTEGER NOT NULL
  )`);
  await query(`CREATE TABLE IF NOT EXISTS connector_accounts (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    provider TEXT NOT NULL,
    refresh_token_enc TEXT,
    connected_at TIMESTAMPTZ NOT NULL,
    status TEXT NOT NULL DEFAULT 'connected',
    UNIQUE(user_id, provider)
  )`);
  await query(`CREATE TABLE IF NOT EXISTS music_tracks (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title TEXT NOT NULL DEFAULT '',
    filename TEXT NOT NULL DEFAULT '',
    mime TEXT NOT NULL DEFAULT 'audio/wav',
    size_bytes INTEGER NOT NULL DEFAULT 0,
    audio_b64 TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL,
    deleted_at TIMESTAMPTZ
  )`);
  // Pocket Family, Calendar on Today, device backups (scram-api-v25-guide).
  await query(`ALTER TABLE device_links ADD COLUMN IF NOT EXISTS backup_requested_at TIMESTAMPTZ`);
  await query(`ALTER TABLE cloud_lists ADD COLUMN IF NOT EXISTS family_id TEXT`);
  await query(`CREATE TABLE IF NOT EXISTS families (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    owner_user_id TEXT NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE,
    join_code TEXT,
    join_code_expires_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL
  )`);
  await query(`CREATE UNIQUE INDEX IF NOT EXISTS idx_families_join_code ON families(join_code)`);
  await query(`CREATE TABLE IF NOT EXISTS family_members (
    user_id TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    family_id TEXT NOT NULL REFERENCES families(id) ON DELETE CASCADE,
    role TEXT NOT NULL DEFAULT 'member',
    joined_at TIMESTAMPTZ NOT NULL
  )`);
  await query(`CREATE INDEX IF NOT EXISTS idx_family_members_family ON family_members(family_id)`);
  await query(`CREATE TABLE IF NOT EXISTS calendar_feeds (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    label TEXT NOT NULL,
    url TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL
  )`);
  await query(`CREATE TABLE IF NOT EXISTS device_backups (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_id TEXT NOT NULL,
    device_name TEXT NOT NULL DEFAULT 'Pocket',
    created_at TIMESTAMPTZ NOT NULL,
    payload_json TEXT NOT NULL,
    size_bytes INTEGER NOT NULL
  )`);
  await query(`CREATE INDEX IF NOT EXISTS idx_device_backups_user_created ON device_backups(user_id, created_at DESC)`);
  // Reading library (`books`) and device_links.volume_percent/brightness_percent were applied
  // directly against the database once (not re-run here — see cloud/neon-fn/README.md) rather
  // than added as ALTER/CREATE statements in this function.
}

const STRIPE_SETTING_KEYS = [
  "stripe_secret_key",
  "stripe_webhook_secret",
  "stripe_price_monthly_id",
  "stripe_product_name",
];

function maskSecret(value) {
  const s = String(value || "");
  if (!s) return null;
  if (s.length <= 8) return "••••••••";
  return s.slice(0, 7) + "…" + s.slice(-4);
}

async function getAppSetting(key) {
  const rows = await query(`SELECT value FROM app_settings WHERE key='${esc(key)}'`);
  return rows[0]?.value ?? "";
}

async function setAppSetting(key, value, actorId) {
  const now = new Date().toISOString();
  const existing = await query(`SELECT key FROM app_settings WHERE key='${esc(key)}'`);
  if (existing.length) {
    await query(
      `UPDATE app_settings SET value=${sqlStr(value)}, updated_at='${now}', updated_by=${sqlNullable(actorId)} WHERE key='${esc(key)}'`,
    );
  } else {
    await query(
      `INSERT INTO app_settings (key,value,updated_at,updated_by) VALUES ('${esc(key)}',${sqlStr(value)},'${now}',${sqlNullable(actorId)})`,
    );
  }
}

async function loadStripeConfig() {
  const fromDb = {};
  for (const k of STRIPE_SETTING_KEYS) {
    fromDb[k] = await getAppSetting(k);
  }
  const secret =
    fromDb.stripe_secret_key ||
    (process.env.STRIPE_SECRET_KEY || "").trim();
  const webhook =
    fromDb.stripe_webhook_secret ||
    (process.env.STRIPE_WEBHOOK_SECRET || "").trim();
  const priceId =
    fromDb.stripe_price_monthly_id ||
    (process.env.STRIPE_PRICE_MONTHLY_ID || "").trim();
  const productName =
    fromDb.stripe_product_name ||
    (process.env.STRIPE_PRODUCT_NAME || "Pocket Cloud").trim() ||
    "Pocket Cloud";
  return {
    secretKey: secret,
    webhookSecret: webhook,
    priceMonthlyId: priceId,
    productName,
    source: fromDb.stripe_secret_key ? "admin" : secret ? "env" : "none",
    mockMode: !secret,
  };
}

function formBody(params) {
  return Object.entries(params)
    .filter(([, v]) => v !== undefined && v !== null && v !== "")
    .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(String(v))}`)
    .join("&");
}

async function stripeFetch(secretKey, method, path, params) {
  const res = await fetch(`https://api.stripe.com/v1${path}`, {
    method,
    headers: {
      Authorization: `Bearer ${secretKey}`,
      "Content-Type": "application/x-www-form-urlencoded",
    },
    body: method === "GET" ? undefined : formBody(params || {}),
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) {
    const msg = data?.error?.message || `Stripe error (${res.status})`;
    const err = new Error(msg);
    err.status = res.status;
    throw err;
  }
  return data;
}

async function upsertSubscriptionMirror(userId, fields) {
  const now = new Date().toISOString();
  const existing = await query(`SELECT user_id FROM subscription_mirrors WHERE user_id='${esc(userId)}'`);
  const status = fields.status || "free";
  const subId = fields.stripe_subscription_id;
  const trialEnds = fields.trial_ends_at;
  const periodEnd = fields.current_period_end;
  const cancel = fields.cancel_at_period_end ? 1 : 0;
  if (existing.length) {
    const sets = [`status='${esc(status)}'`, `updated_at='${now}'`, `cancel_at_period_end=${cancel}`];
    if (subId !== undefined) sets.push(`stripe_subscription_id=${sqlNullable(subId)}`);
    if (trialEnds !== undefined) sets.push(`trial_ends_at=${sqlNullable(trialEnds)}`);
    if (periodEnd !== undefined) sets.push(`current_period_end=${sqlNullable(periodEnd)}`);
    await query(`UPDATE subscription_mirrors SET ${sets.join(",")} WHERE user_id='${esc(userId)}'`);
  } else {
    await query(
      `INSERT INTO subscription_mirrors (user_id,stripe_subscription_id,status,trial_ends_at,current_period_end,cancel_at_period_end,updated_at) VALUES ('${esc(userId)}',${sqlNullable(subId)},'${esc(status)}',${sqlNullable(trialEnds)},${sqlNullable(periodEnd)},${cancel},'${now}')`,
    );
  }
}

async function completeMockCheckout(checkoutId) {
  const rows = await query(`SELECT * FROM mock_checkouts WHERE id='${esc(checkoutId)}'`);
  const row = rows[0];
  if (!row || row.completed_at) return;
  const now = new Date();
  const trialEnds = new Date(now.getTime() + 7 * 86400000);
  const withTrial = Number(row.with_trial) === 1;
  const periodEnd = withTrial ? trialEnds : new Date(now.getTime() + 30 * 86400000);
  const mockCustomer = `cus_mock_${String(row.user_id).replace(/-/g, "").slice(0, 14)}`;
  const mockSub = `sub_mock_${newId().replace(/-/g, "").slice(0, 14)}`;
  await query(`UPDATE users SET stripe_customer_id='${esc(mockCustomer)}'${withTrial ? ", trial_consumed=1" : ""} WHERE id='${esc(row.user_id)}'`);
  await upsertSubscriptionMirror(row.user_id, {
    stripe_subscription_id: mockSub,
    status: withTrial ? "trialing" : "active",
    trial_ends_at: withTrial ? trialEnds.toISOString() : null,
    current_period_end: periodEnd.toISOString(),
    cancel_at_period_end: false,
  });
  await query(`UPDATE mock_checkouts SET completed_at='${now.toISOString()}' WHERE id='${esc(checkoutId)}'`);
}

async function resolveUserIdFromCustomer(customerId) {
  if (!customerId) return null;
  const rows = await query(`SELECT id FROM users WHERE stripe_customer_id='${esc(customerId)}'`);
  return rows[0]?.id || null;
}

async function handleStripeWebhookEvent(event) {
  const existing = await query(`SELECT id FROM stripe_events WHERE id='${esc(event.id)}'`);
  if (existing.length) return;
  const type = event.type;
  const obj = event.data?.object || {};
  if (type === "checkout.session.completed") {
    const userId = obj.client_reference_id;
    if (userId) {
      if (typeof obj.customer === "string") {
        await query(`UPDATE users SET stripe_customer_id='${esc(obj.customer)}' WHERE id='${esc(userId)}'`);
      }
      if (obj.mode === "subscription") {
        await upsertSubscriptionMirror(userId, {
          stripe_subscription_id: typeof obj.subscription === "string" ? obj.subscription : null,
          status: "trialing",
          trial_ends_at: new Date(Date.now() + 7 * 86400000).toISOString(),
          current_period_end: new Date(Date.now() + 7 * 86400000).toISOString(),
        });
        await query(`UPDATE users SET trial_consumed=1 WHERE id='${esc(userId)}'`);
      }
    }
  } else if (type === "customer.subscription.created" || type === "customer.subscription.updated") {
    const customerId = typeof obj.customer === "string" ? obj.customer : null;
    const userId = await resolveUserIdFromCustomer(customerId);
    if (userId) {
      await upsertSubscriptionMirror(userId, {
        stripe_subscription_id: obj.id,
        status: obj.status,
        trial_ends_at: obj.trial_end ? new Date(obj.trial_end * 1000).toISOString() : null,
        current_period_end: obj.current_period_end
          ? new Date(obj.current_period_end * 1000).toISOString()
          : null,
        cancel_at_period_end: Boolean(obj.cancel_at_period_end),
      });
      if (obj.status === "trialing") {
        await query(`UPDATE users SET trial_consumed=1 WHERE id='${esc(userId)}'`);
      }
    }
  } else if (type === "customer.subscription.deleted") {
    const customerId = typeof obj.customer === "string" ? obj.customer : null;
    const userId = await resolveUserIdFromCustomer(customerId);
    if (userId) {
      await upsertSubscriptionMirror(userId, {
        stripe_subscription_id: obj.id,
        status: "canceled",
        trial_ends_at: null,
        current_period_end: obj.current_period_end
          ? new Date(obj.current_period_end * 1000).toISOString()
          : null,
        cancel_at_period_end: false,
      });
    }
  } else if (type === "invoice.paid" || type === "invoice.payment_succeeded") {
    const customerId = typeof obj.customer === "string" ? obj.customer : null;
    const userId = await resolveUserIdFromCustomer(customerId);
    const subId = typeof obj.subscription === "string" ? obj.subscription : null;
    if (userId && subId) {
      const periodEnd = obj.lines?.data?.[0]?.period?.end;
      await upsertSubscriptionMirror(userId, {
        stripe_subscription_id: subId,
        status: "active",
        current_period_end: periodEnd ? new Date(periodEnd * 1000).toISOString() : undefined,
      });
    }
  } else if (type === "invoice.payment_failed" || type === "invoice.payment_action_required") {
    const customerId = typeof obj.customer === "string" ? obj.customer : null;
    const userId = await resolveUserIdFromCustomer(customerId);
    if (userId) {
      await upsertSubscriptionMirror(userId, {
        stripe_subscription_id: typeof obj.subscription === "string" ? obj.subscription : null,
        status: "past_due",
      });
    }
  }
  const now = new Date().toISOString();
  await query(`INSERT INTO stripe_events (id, received_at) VALUES ('${esc(event.id)}','${now}')`);
}

function verifyStripeWebhookSignature(payload, header, secret) {
  if (!secret || !header) return false;
  const parts = Object.fromEntries(
    String(header)
      .split(",")
      .map((p) => p.trim().split("="))
      .filter((p) => p.length === 2),
  );
  const ts = parts.t;
  const v1 = parts.v1;
  if (!ts || !v1) return false;
  const age = Math.abs(Date.now() / 1000 - Number(ts));
  if (!Number.isFinite(age) || age > 300) return false;
  const signed = createHmac("sha256", secret).update(`${ts}.${payload}`, "utf8").digest("hex");
  try {
    return timingSafeEqual(Buffer.from(signed, "hex"), Buffer.from(v1, "hex"));
  } catch {
    return false;
  }
}

let schemaReady = null;
function readySchema() {
  if (!schemaReady) schemaReady = ensureAdminSchema().catch((e) => { schemaReady = null; throw e; });
  return schemaReady;
}


let selfSourceSha256 = null;
async function getSelfSourceSha256() {
  if (selfSourceSha256) return selfSourceSha256;
  try {
    const { readFileSync } = await import("fs");
    const { fileURLToPath } = await import("url");
    const bytes = readFileSync(fileURLToPath(import.meta.url));
    selfSourceSha256 = createHash("sha256").update(bytes).digest("hex");
  } catch (e) {
    selfSourceSha256 = "unavailable:" + String(e && e.message || e);
  }
  return selfSourceSha256;
}

/** Wrap raw PCM s16le mono as a minimal WAV so Whisper multipart accepts it. */
function pcmS16leToWav(pcm, sampleRate) {
  const dataSize = pcm.length;
  const buf = Buffer.alloc(44 + dataSize);
  buf.write("RIFF", 0);
  buf.writeUInt32LE(36 + dataSize, 4);
  buf.write("WAVE", 8);
  buf.write("fmt ", 12);
  buf.writeUInt32LE(16, 16); // PCM fmt chunk size
  buf.writeUInt16LE(1, 20); // PCM
  buf.writeUInt16LE(1, 22); // mono
  buf.writeUInt32LE(sampleRate, 24);
  buf.writeUInt32LE(sampleRate * 2, 28); // byte rate
  buf.writeUInt16LE(2, 32); // block align
  buf.writeUInt16LE(16, 34); // bits
  buf.write("data", 36);
  buf.writeUInt32LE(dataSize, 40);
  pcm.copy(buf, 44);
  return buf;
}

const STT_SETTING_KEYS = ["stt_openai_api_key", "stt_groq_api_key"];

async function loadSttSecrets() {
  let openai = (process.env.OPENAI_API_KEY || process.env.STT_API_KEY || "").trim();
  let groq = (process.env.GROQ_API_KEY || "").trim();
  let proxy = (process.env.STT_PROXY_URL || "").trim();
  let proxyKey = (process.env.STT_PROXY_KEY || "").trim();
  try {
    if (!openai) openai = String(await getAppSetting("stt_openai_api_key") || "").trim();
    if (!groq) groq = String(await getAppSetting("stt_groq_api_key") || "").trim();
    if (!proxy) proxy = String(await getAppSetting("stt_proxy_url") || "").trim();
    if (!proxyKey) proxyKey = String(await getAppSetting("stt_proxy_key") || "").trim();
  } catch {
    /* schema may not be ready in health probes; ignore */
  }
  return { openai, groq, proxy, proxyKey };
}

function extractChatTranscript(parsed) {
  const choice = parsed?.choices?.[0]?.message?.content;
  if (typeof choice === "string") return choice.trim();
  if (Array.isArray(choice)) {
    return choice
      .map((p) => (typeof p?.text === "string" ? p.text : typeof p === "string" ? p : ""))
      .join(" ")
      .trim();
  }
  const parts = parsed?.candidates?.[0]?.content?.parts;
  if (Array.isArray(parts)) {
    return parts.map((p) => (typeof p?.text === "string" ? p.text : "")).join(" ").trim();
  }
  if (typeof parsed?.text === "string") return parsed.text.trim();
  return "";
}

/**
 * Cloud STT for device dictation. Tries, in order:
 *   1) OPENAI_API_KEY / app_settings → OpenAI whisper-1
 *   2) GROQ_API_KEY / app_settings → Groq whisper-large-v3
 *   3) STT_PROXY_URL → POST WAV, expect {text}
 *   4) Cloudflare Workers AI (CLOUDFLARE_ACCOUNT_ID + CLOUDFLARE_API_TOKEN)
 *   5) Neon AI Gateway Gemini multimodal (injected NEON_AI_GATEWAY_*)
 */
async function transcribePcmS16le(pcm, sampleRate) {
  const wav = pcmS16leToWav(pcm, sampleRate);
  const wavB64 = wav.toString("base64");
  const { openai: openaiKey, groq: groqKey, proxy, proxyKey } = await loadSttSecrets();
  const errors = [];

  async function tryWhisperForm(url, key, model) {
    const form = new FormData();
    form.append("file", new Blob([wav], { type: "audio/wav" }), "note.wav");
    form.append("model", model);
    form.append("response_format", "json");
    form.append("language", "en");
    const r = await fetch(url, {
      method: "POST",
      headers: { Authorization: `Bearer ${key}` },
      body: form,
    });
    const bodyText = await r.text();
    if (!r.ok) throw new Error(`${url} → HTTP ${r.status}: ${bodyText.slice(0, 180)}`);
    let parsed = {};
    try {
      parsed = JSON.parse(bodyText);
    } catch {
      throw new Error(`${url} → non-JSON`);
    }
    const text = typeof parsed.text === "string" ? parsed.text.trim() : "";
    if (!text) throw new Error(`${url} → empty text`);
    return text;
  }

  if (openaiKey) {
    try {
      return await tryWhisperForm("https://api.openai.com/v1/audio/transcriptions", openaiKey, "whisper-1");
    } catch (e) {
      errors.push(String(e && e.message || e));
    }
  }
  if (groqKey) {
    try {
      return await tryWhisperForm(
        "https://api.groq.com/openai/v1/audio/transcriptions",
        groqKey,
        "whisper-large-v3",
      );
    } catch (e) {
      errors.push(String(e && e.message || e));
    }
  }

  const proxyUrl = (proxy || "").trim().replace(/\/$/, "");
  if (proxyUrl) {
    try {
      const headers = { "content-type": "application/octet-stream", accept: "application/json" };
      if (proxyKey) headers["x-stt-key"] = proxyKey;
      const r = await fetch(proxyUrl, { method: "POST", headers, body: wav });
      const bodyText = await r.text();
      if (!r.ok) throw new Error(`STT_PROXY_URL → HTTP ${r.status}: ${bodyText.slice(0, 180)}`);
      const parsed = JSON.parse(bodyText);
      const text = typeof parsed.text === "string" ? parsed.text.trim() : "";
      if (!text) throw new Error("STT_PROXY_URL → empty text");
      return text;
    } catch (e) {
      errors.push(String(e && e.message || e));
    }
  }

  const cfAccount = (process.env.CLOUDFLARE_ACCOUNT_ID || "").trim();
  const cfToken = (process.env.CLOUDFLARE_API_TOKEN || "").trim();
  if (cfAccount && cfToken) {
    try {
      const url = `https://api.cloudflare.com/client/v4/accounts/${cfAccount}/ai/run/@cf/openai/whisper-large-v3-turbo`;
      const r = await fetch(url, {
        method: "POST",
        headers: { Authorization: `Bearer ${cfToken}`, "content-type": "application/json" },
        body: JSON.stringify({ audio: wavB64, task: "transcribe", language: "en" }),
      });
      const bodyText = await r.text();
      if (!r.ok) throw new Error(`Cloudflare AI → HTTP ${r.status}: ${bodyText.slice(0, 180)}`);
      const parsed = JSON.parse(bodyText);
      const text = typeof parsed?.result?.text === "string"
        ? parsed.result.text.trim()
        : typeof parsed?.text === "string"
          ? parsed.text.trim()
          : "";
      if (!text) throw new Error("Cloudflare AI → empty text");
      return text;
    } catch (e) {
      errors.push(String(e && e.message || e));
    }
  }

  // Neon AI Gateway: no Whisper route (404). Use Gemini multimodal audio → text instead.
  const neonTok = (process.env.NEON_AI_GATEWAY_TOKEN || "").trim();
  const neonBase = (process.env.NEON_AI_GATEWAY_BASE_URL || "").replace(/\/$/, "");
  if (neonTok && neonBase) {
    const prompt =
      "Transcribe any spoken words in this audio. Reply with only the transcript text. " +
      "If there is no speech, reply with exactly EMPTY.";
    const geminiModels = ["gemini-3-flash", "gemini-3-5-flash-lite", "gemini-2-5-flash"];
    for (const model of geminiModels) {
      try {
        const r = await fetch(`${neonBase}/v1/chat/completions`, {
          method: "POST",
          headers: {
            Authorization: `Bearer ${neonTok}`,
            "content-type": "application/json",
          },
          body: JSON.stringify({
            model,
            messages: [
              {
                role: "user",
                content: [
                  { type: "text", text: prompt },
                  { type: "input_audio", input_audio: { data: wavB64, format: "wav" } },
                ],
              },
            ],
          }),
        });
        const bodyText = await r.text();
        if (!r.ok) throw new Error(`Neon Gemini chat ${model} → HTTP ${r.status}: ${bodyText.slice(0, 180)}`);
        const parsed = JSON.parse(bodyText);
        let text = extractChatTranscript(parsed);
        if (/^EMPTY$/i.test(text)) return ""; // reachable, no speech
        if (text) return text;
        throw new Error(`Neon Gemini chat ${model} → empty transcript`);
      } catch (e) {
        errors.push(String(e && e.message || e));
      }
      try {
        const r = await fetch(`${neonBase}/gemini/v1beta/models/${model}:generateContent`, {
          method: "POST",
          headers: {
            Authorization: `Bearer ${neonTok}`,
            "content-type": "application/json",
          },
          body: JSON.stringify({
            contents: [
              {
                role: "user",
                parts: [
                  { text: prompt },
                  { inline_data: { mime_type: "audio/wav", data: wavB64 } },
                ],
              },
            ],
          }),
        });
        const bodyText = await r.text();
        if (!r.ok) throw new Error(`Neon Gemini native ${model} → HTTP ${r.status}: ${bodyText.slice(0, 180)}`);
        const parsed = JSON.parse(bodyText);
        let text = extractChatTranscript(parsed);
        if (/^EMPTY$/i.test(text)) return ""; // reachable, no speech
        if (text) return text;
        throw new Error(`Neon Gemini native ${model} → empty transcript`);
      } catch (e) {
        errors.push(String(e && e.message || e));
      }
    }
  }

  if (!errors.length) {
    throw new Error(
      "No STT provider configured (set GROQ_API_KEY or OPENAI_API_KEY on the Neon Function, or Admin → STT)",
    );
  }
  throw new Error(errors.join(" | ") || "STT failed");
}

export default {
  async fetch(req) {
    if (req.method === "OPTIONS") return new Response(null, { status: 204, headers: cors(req) });
    const u = new URL(req.url), path = u.pathname;
    // Health does not depend on DB schema readiness so it stays useful for diagnosing DB-layer issues.
    if (path === "/health" || path === "/" || path === "/v1/health") {
      const sha256 = await getSelfSourceSha256();
      return json(req, { ok: true, build: "scram-api-v25-guide", sourceSha256: sha256 });
    }
    try {
      await readySchema();

      if (req.method === "POST" && path === "/v1/auth/register") {
        const body = await req.json().catch(() => ({}));
        const email = typeof body.email === "string" ? body.email.trim().toLowerCase() : "";
        const password = typeof body.password === "string" ? body.password : "";
        if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) return json(req, { message: "Enter a valid email address." }, 400);
        if (password.length < 8) return json(req, { message: "Password must be at least 8 characters." }, 400);
        const existing = await query(`SELECT id FROM users WHERE email='${esc(email)}'`);
        if (existing.length) return json(req, { message: "An account with that email already exists. Sign in instead." }, 400);
        const id = newId(), now = new Date().toISOString(), ph = hashPassword(password);
        // For now: no email verification — mark verified at create so sign-in works immediately.
        await query(`INSERT INTO users (id,email,password_hash,email_verified_at,created_at,trial_consumed,stripe_customer_id,had_subscription) VALUES ('${esc(id)}','${esc(email)}','${esc(ph)}','${now}','${now}',0,NULL,0)`);
        return json(req, { ok: true, message: "Account created. You can sign in now." });
      }

      if (req.method === "GET" && path === "/v1/auth/verify-email") {
        // Legacy links: email verification is not required. Send them to sign in.
        return Response.redirect(`${PWA_ORIGIN}/login`, 302);
      }

      if (req.method === "POST" && path === "/v1/auth/login") {
        const body = await req.json().catch(() => ({}));
        const email = typeof body.email === "string" ? body.email.trim().toLowerCase() : "";
        const password = typeof body.password === "string" ? body.password : "";
        const users = await query(`SELECT * FROM users WHERE email='${esc(email)}'`);
        const user = users[0];
        if (!user?.password_hash || !verifyPassword(password, user.password_hash)) {
          return json(req, { message: "Wrong email or password." }, 401);
        }
        if (user.disabled_at) {
          return json(req, { message: "This account is disabled." }, 403);
        }
        const token = randomToken(32);
        const nowIso = new Date().toISOString();
        const expires = new Date(Date.now() + 30 * 86400e3).toISOString();
        // Backfill verified_at for any older accounts that never clicked a link.
        if (!user.email_verified_at) {
          await query(`UPDATE users SET email_verified_at='${nowIso}' WHERE id='${esc(user.id)}'`);
        }
        await query(`INSERT INTO sessions (id,user_id,token_hash,created_at,expires_at) VALUES ('${esc(newId())}','${esc(user.id)}','${esc(sha256Hex(token))}','${nowIso}','${expires}')`);
        await query(`UPDATE users SET last_login_at='${nowIso}' WHERE id='${esc(user.id)}'`);
        // session_token in body: required on mobile where cross-site cookies are blocked.
        return json(
          req,
          {
            ok: true,
            session_token: token,
            user: { id: user.id, email: user.email, role: user.role || "user", is_admin: isAdminUser(user) },
          },
          200,
          { "set-cookie": sessionCookie(token, 30 * 86400) },
        );
      }

      // "Continue with Northwork": one Northwork account for Pocket too (accounts matched by email).
      if (req.method === "POST" && path === "/v1/auth/northwork") {
        const body = await req.json().catch(() => ({}));
        const pass = await checkNorthworkPass(body.token);
        if (!pass) return json(req, { message: "Your Northwork sign-in expired. Please try again." }, 401);
        const email = pass.email.trim().toLowerCase();
        const nowIso = new Date().toISOString();
        let user = (await query(`SELECT * FROM users WHERE email='${esc(email)}'`))[0];
        if (!user) {
          const id = newId();
          await query(`INSERT INTO users (id,email,password_hash,email_verified_at,created_at,trial_consumed,stripe_customer_id,had_subscription) VALUES ('${esc(id)}','${esc(email)}',NULL,'${nowIso}','${nowIso}',0,NULL,0)`);
          user = (await query(`SELECT * FROM users WHERE id='${esc(id)}'`))[0];
        }
        if (user.disabled_at) return json(req, { message: "This account is disabled." }, 403);
        if (!user.email_verified_at) await query(`UPDATE users SET email_verified_at='${nowIso}' WHERE id='${esc(user.id)}'`);
        const token = randomToken(32);
        const expires = new Date(Date.now() + 30 * 86400e3).toISOString();
        await query(`INSERT INTO sessions (id,user_id,token_hash,created_at,expires_at) VALUES ('${esc(newId())}','${esc(user.id)}','${esc(sha256Hex(token))}','${nowIso}','${expires}')`);
        await query(`UPDATE users SET last_login_at='${nowIso}' WHERE id='${esc(user.id)}'`);
        return json(
          req,
          { ok: true, session_token: token, user: { id: user.id, email: user.email, role: user.role || "user", is_admin: isAdminUser(user) } },
          200,
          { "set-cookie": sessionCookie(token, 30 * 86400) },
        );
      }

      if (req.method === "POST" && path === "/v1/auth/logout") {
        const headerTok = (req.headers.get("x-pocket-session") || "").trim();
        const cookieTok = getCookie(req, SESSION_COOKIE);
        const token = headerTok || cookieTok;
        if (token) await query(`DELETE FROM sessions WHERE token_hash='${esc(sha256Hex(token))}'`);
        return json(req, { ok: true }, 200, {
          "set-cookie": `${SESSION_COOKIE}=; Path=/; HttpOnly; Secure; SameSite=None; Max-Age=0`,
        });
      }

      if (req.method === "GET" && path === "/v1/me") {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        if (user.disabled_at) return json(req, { message: "This account is disabled." }, 403);
        const devices = await query(`SELECT id, device_id, device_name, linked_at, last_seen_at FROM device_links WHERE user_id='${esc(user.id)}' ORDER BY linked_at DESC`);
        const subs = await query(`SELECT status, trial_ends_at, current_period_end FROM subscription_mirrors WHERE user_id='${esc(user.id)}'`);
        const sub = subs[0];
        const entitled = sub?.status === "active" || sub?.status === "trialing";
        return json(req, {
          user: {
            id: user.id,
            email: user.email,
            created_at: user.created_at,
            trial_consumed: Boolean(Number(user.trial_consumed)),
            email_verified: Boolean(user.email_verified_at),
            role: user.role || "user",
            is_admin: isAdminUser(user),
            disabled: Boolean(user.disabled_at),
            last_login_at: user.last_login_at || null,
          },
          entitlement: {
            status: sub?.status || "free",
            entitled: Boolean(entitled),
            trial_consumed: Boolean(Number(user.trial_consumed)),
            trial_ends_at: sub?.trial_ends_at || null,
            current_period_end: sub?.current_period_end || null,
            has_customer: Boolean(user.stripe_customer_id),
          },
          devices,
        });
      }

      if (req.method === "POST" && path === "/v1/pair/sessions") {
        // Prefer x-device-key — Neon Functions may strip Authorization.
        const key = (req.headers.get("x-device-key") || "").trim();
        if (!safeEqual(key, DEVICE_API_KEY)) return json(req, { message: "Sign in to continue." }, 401);
        const body = await req.json().catch(() => ({}));
        const device_id = typeof body.device_id === "string" ? body.device_id : "";
        const code_public = typeof body.code_public === "string" ? body.code_public : typeof body.code === "string" ? body.code : "";
        const code = normalizePairCode(code_public);
        if (!device_id.trim() || !isValidPairCode(code)) return json(req, { message: "That pairing code isn't valid." }, 400);
        const now = new Date(), expires = new Date(now.getTime() + 10 * 60e3);
        await query(`UPDATE pair_sessions SET status='expired' WHERE device_id='${esc(device_id)}' AND status='pending'`);
        await query(`INSERT INTO pair_sessions (id,device_id,code_hash,code_public_hint,expires_at,created_at,status) VALUES ('${esc(newId())}','${esc(device_id)}','${esc(sha256Hex(code))}','${esc(code.slice(0, 2) + "******")}','${expires.toISOString()}','${now.toISOString()}','pending')`);
        return json(req, { ok: true, expires_at: expires.toISOString() }, 201);
      }

      // ---- Apps (pocket-contract v1.0 §C–E) ----------------------------------------
      if (req.method === "GET" && path === "/v1/apps") {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        await maybePollRegistry();
        const reg = await registryApps();
        const installs = await appInstalls(user.id);
        const installed = [];
        for (const r of installs) {
          const app = reg.get(r.app_id);
          if (!app) continue;
          let auth_connected = true;
          if (app.auth.type === "oauth2") {
            const a = await appAuthRow(user.id, app.auth.provider);
            auth_connected = Boolean(a && a.status === "ok");
          }
          installed.push({ id: app.id, version: app.version, name: app.name, devices: wallDevices(r), auth_connected });
        }
        const available = [...reg.values()].map((a) => ({
          id: a.id, version: a.version, name: a.name, summary: a.summary, developer: a.developer,
          icon_pbm_b64: a.icon_pbm_b64, auth_type: a.auth.type, surfaces: a.surfaces,
        }));
        return json(req, { installed, available });
      }

      if (req.method === "POST" && (path === "/v1/apps/install" || path === "/v1/apps/uninstall")) {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const body = await req.json().catch(() => ({}));
        const id = typeof body.id === "string" ? body.id : "";
        if (path === "/v1/apps/uninstall") {
          await query(`DELETE FROM app_installs WHERE user_id='${esc(user.id)}' AND app_id='${esc(id)}'`);
          await query(`DELETE FROM app_feeds WHERE user_id='${esc(user.id)}' AND app_id='${esc(id)}'`);
          return json(req, { ok: true });
        }
        await maybePollRegistry();
        const app = (await registryApps()).get(id);
        if (!app) return json(req, { message: "That app isn't available.", code: "not_found" }, 404);
        const devices = await resolveWallDevices(user.id, body.devices);
        if (!devices) return json(req, { message: "Choose one of your own Pockets.", code: "device_not_yours" }, 403);
        const others = (await appInstalls(user.id)).filter((r) => r.app_id !== id);
        const owned = (await query(`SELECT device_id FROM device_links WHERE user_id='${esc(user.id)}'`)).map((r) => r.device_id);
        const full = (devices === "all" ? owned : devices).filter((d) => others.filter((r) => wallTargets(r, d)).length >= APP_MAX_PER_DEVICE);
        if (full.length) return json(req, { message: "A Pocket can show 6 apps. Remove one first.", code: "too_many", devices: full }, 409);
        await query(`DELETE FROM app_installs WHERE user_id='${esc(user.id)}' AND app_id='${esc(id)}'`);
        await query(`INSERT INTO app_installs (user_id,app_id,devices_json,installed_at) VALUES ('${esc(user.id)}','${esc(id)}',${sqlStr(JSON.stringify(devices))},'${new Date().toISOString()}')`);
        if (app.auth.type === "oauth2") {
          const a = await appAuthRow(user.id, app.auth.provider);
          if (!a || a.status !== "ok") {
            const auth_url = await appAuthUrl(user.id, app, appReturnUrl(body.return));
            if (!auth_url) return json(req, { ok: true, needs_auth: true, message: "Sign-in for this app isn't available yet.", code: "auth_unavailable" });
            return json(req, { ok: true, needs_auth: true, auth_url });
          }
        }
        return json(req, { ok: true, needs_auth: false });
      }

      const appAuthStart = path.match(/^\/v1\/apps\/([a-z0-9.-]+)\/auth\/start$/);
      if (appAuthStart && (req.method === "POST" || req.method === "GET")) {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const app = (await registryApps()).get(appAuthStart[1]);
        if (!app || app.auth.type !== "oauth2") return json(req, { message: "That app doesn't need a sign-in.", code: "not_found" }, 404);
        const body = req.method === "POST" ? await req.json().catch(() => ({})) : {};
        const auth_url = await appAuthUrl(user.id, app, appReturnUrl(body.return || u.searchParams.get("return")));
        if (!auth_url) return json(req, { message: "Sign-in for this app isn't available yet.", code: "auth_unavailable" }, 503);
        return json(req, { auth_url });
      }

      // OAuth callback relay: northwork.app/pocket/app/oauth/<provider>/callback posts {code, state} here.
      const appCb = path.match(/^\/v1\/apps\/oauth\/([a-z0-9-]+)\/callback$/);
      if (req.method === "POST" && appCb) {
        const provider = appCb[1];
        const body = await req.json().catch(() => ({}));
        const state = typeof body.state === "string" ? body.state : "";
        const rows = state ? await query(`SELECT * FROM app_oauth_states WHERE state='${esc(state)}' AND provider='${esc(provider)}'`) : [];
        const st = rows[0];
        if (!st) return json(req, { ok: false, message: "That sign-in link expired. Try again from Pocket.", return_url: APP_DEFAULT_RETURN }, 400);
        await query(`DELETE FROM app_oauth_states WHERE state='${esc(state)}'`);
        if (Date.now() - Date.parse(st.created_at) > 3600e3 || typeof body.code !== "string" || !body.code) {
          return json(req, { ok: false, message: body.error ? "Sign-in was cancelled." : "That sign-in link expired. Try again from Pocket.", return_url: st.return_url }, 400);
        }
        const cfg = await oauthConfig(provider);
        if (!cfg) return json(req, { ok: false, message: "Sign-in isn't available right now.", return_url: st.return_url }, 503);
        try {
          const r = await oauthTokenRequest(cfg, { grant_type: "authorization_code", code: body.code, redirect_uri: cfg.redirect, code_verifier: st.verifier });
          if (!r.ok || !r.body.access_token) return json(req, { ok: false, message: "Sign-in didn't finish. Try again.", return_url: st.return_url }, 400);
          await storeAppTokens(st.user_id, provider, r.body);
          await query(`DELETE FROM app_feeds WHERE user_id='${esc(st.user_id)}' AND app_id='${esc(st.app_id)}'`);
        } catch {
          return json(req, { ok: false, message: "Sign-in didn't finish. Try again.", return_url: st.return_url }, 502);
        }
        return json(req, { ok: true, return_url: st.return_url });
      }

      if (req.method === "GET" && path === "/v1/device/apps") {
        const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
        const link = await deviceLinkForRequest(req, deviceId, { strict: true });
        if (!link) return json(req, { message: "Unauthorized.", code: "device_token_required" }, 401);
        await maybePollRegistry();
        const apps = await deviceAppsPayload(link.user_id, deviceId);
        return json(req, { apps });
      }

      // ---- Wallpapers (pocket-contract v1.0 §B) --------------------------------------
      if (path === "/v1/wallpapers" && (req.method === "GET" || req.method === "POST")) {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const rows = await wallpapersForUser(user.id);
        if (req.method === "GET") return json(req, { wallpapers: rows.map((r) => serializeWallpaper(req, r)) });
        const raw = await req.text();
        if (raw.length > 80 * 1024) return json(req, { message: "That wallpaper is too large.", code: "too_large" }, 413);
        let body = {};
        try { body = JSON.parse(raw || "{}"); } catch { return json(req, { message: "That request wasn't valid JSON." }, 400); }
        const name = typeof body.name === "string" ? body.name.trim() : "";
        if (!name || name.length > 40) return json(req, { message: "Give the wallpaper a name (up to 40 characters).", code: "bad_name" }, 400);
        const pbm = parseWallPbm(body.image_pbm_b64);
        if (!pbm) return json(req, { message: "The image must be a 480×800 black-and-white PBM (P4).", code: "bad_pbm" }, 400);
        const layout = parseWallLayout(body.layout);
        if (!layout) return json(req, { message: "The layout isn't valid.", code: "bad_layout" }, 400);
        const devices = await resolveWallDevices(user.id, body.devices);
        if (!devices) return json(req, { message: "Choose one of your own Pockets.", code: "device_not_yours" }, 403);
        const full = await wallOverLimit(user.id, rows, devices, null);
        if (full.length) {
          return json(req, {
            message: "A Pocket can keep 5 wallpapers. Remove one first.",
            code: "too_many",
            devices: full,
            wallpapers: rows.map((r) => serializeWallpaper(req, r)),
          }, 409);
        }
        const id = "wp_" + newId().replace(/-/g, "").slice(0, 20);
        const now = new Date().toISOString();
        const sha = createHash("sha256").update(pbm).digest("hex");
        await query(`INSERT INTO wallpapers (id,user_id,name,image_b64,sha256,layout_json,devices_json,created_at) VALUES ('${esc(id)}','${esc(user.id)}',${sqlStr(name)},'${pbm.toString("base64")}','${sha}',${sqlStr(JSON.stringify(layout))},${sqlStr(JSON.stringify(devices))},'${now}')`);
        return json(req, { ok: true, wallpaper: serializeWallpaper(req, { id, name, sha256: sha, layout_json: JSON.stringify(layout), devices_json: JSON.stringify(devices), created_at: now }) }, 201);
      }

      const wallImg = path.match(/^\/v1\/wallpapers\/([A-Za-z0-9_-]+)\/image\.pbm$/);
      if (req.method === "GET" && wallImg) {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const rows = await query(`SELECT image_b64 FROM wallpapers WHERE id='${esc(wallImg[1])}' AND user_id='${esc(user.id)}'`);
        if (!rows[0]) return json(req, { message: "That wallpaper wasn't found." }, 404);
        const bytes = Buffer.from(rows[0].image_b64, "base64");
        return new Response(bytes, { status: 200, headers: { ...cors(req), "content-type": "image/x-portable-bitmap", "content-length": String(bytes.length), "cache-control": "private, max-age=300" } });
      }

      const wallOne = path.match(/^\/v1\/wallpapers\/([A-Za-z0-9_-]+)$/);
      if (wallOne && (req.method === "PATCH" || req.method === "DELETE")) {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const rows = await wallpapersForUser(user.id);
        const row = rows.find((r) => r.id === wallOne[1]);
        if (!row) return json(req, { message: "That wallpaper wasn't found." }, 404);
        if (req.method === "DELETE") {
          await query(`DELETE FROM wallpapers WHERE id='${esc(row.id)}' AND user_id='${esc(user.id)}'`);
          return json(req, { ok: true });
        }
        const body = await req.json().catch(() => ({}));
        let name = row.name;
        if (body.name !== undefined) {
          name = typeof body.name === "string" ? body.name.trim() : "";
          if (!name || name.length > 40) return json(req, { message: "Give the wallpaper a name (up to 40 characters).", code: "bad_name" }, 400);
        }
        let devices = wallDevices(row);
        if (body.devices !== undefined) {
          devices = await resolveWallDevices(user.id, body.devices);
          if (!devices) return json(req, { message: "Choose one of your own Pockets.", code: "device_not_yours" }, 403);
          const full = await wallOverLimit(user.id, rows, devices, row.id);
          if (full.length) return json(req, { message: "A Pocket can keep 5 wallpapers. Remove one first.", code: "too_many", devices: full }, 409);
        }
        await query(`UPDATE wallpapers SET name=${sqlStr(name)}, devices_json=${sqlStr(JSON.stringify(devices))} WHERE id='${esc(row.id)}'`);
        return json(req, { ok: true, wallpaper: serializeWallpaper(req, { ...row, name, devices_json: JSON.stringify(devices) }) });
      }

      // Device side: needs this Pocket's own token (strict).
      if (req.method === "GET" && path === "/v1/device/wallpapers") {
        const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
        const link = await deviceLinkForRequest(req, deviceId, { strict: true });
        if (!link) return json(req, { message: "Unauthorized.", code: "device_token_required" }, 401);
        const rows = (await wallpapersForUser(link.user_id)).filter((r) => wallTargets(r, deviceId)).slice(0, WALL_MAX_PER_DEVICE);
        return json(req, {
          wallpapers: rows.map((r) => ({ id: r.id, name: r.name, sha256: r.sha256, created_at: r.created_at, layout: wallLayout(r) })),
        });
      }
      const wallDev = path.match(/^\/v1\/device\/wallpaper\/([A-Za-z0-9_-]+)$/);
      if (req.method === "GET" && wallDev) {
        const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
        const link = await deviceLinkForRequest(req, deviceId, { strict: true });
        if (!link) return json(req, { message: "Unauthorized.", code: "device_token_required" }, 401);
        const rows = await query(`SELECT image_b64, devices_json FROM wallpapers WHERE id='${esc(wallDev[1])}' AND user_id='${esc(link.user_id)}'`);
        if (!rows[0] || !wallTargets(rows[0], deviceId)) return json(req, { message: "That wallpaper wasn't found." }, 404);
        const bytes = Buffer.from(rows[0].image_b64, "base64");
        return new Response(bytes, { status: 200, headers: { ...cors(req), "content-type": "application/octet-stream", "content-length": String(bytes.length) } });
      }

      // Per-device token for a Pocket linked before tokens existed. Issued once; after that
      // only re-pairing in Companion issues a new one.
      if (req.method === "POST" && path === "/v1/device/token") {
        const key = (req.headers.get("x-device-key") || "").trim();
        if (!safeEqual(key, DEVICE_API_KEY)) return json(req, { message: "Unauthorized." }, 401);
        const body = await req.json().catch(() => ({}));
        const deviceId = String(body.device_id || req.headers.get("x-device-id") || "").trim();
        if (!deviceId) return json(req, { message: "Missing device." }, 400);
        const rows = await query(`SELECT id, token_issued_at FROM device_links WHERE device_id='${esc(deviceId)}' ORDER BY linked_at DESC LIMIT 1`);
        const link = rows[0];
        if (!link) return json(req, { message: "This Pocket isn't linked.", code: "not_linked" }, 404);
        if (link.token_issued_at) {
          return json(req, { message: "Link this Pocket again in Companion.", code: "token_already_issued" }, 409);
        }
        const token = randomToken(32);
        const now = new Date().toISOString();
        const upd = await query(`UPDATE device_links SET device_token_hash='${esc(sha256Hex(token))}', token_issued_at='${now}' WHERE id='${esc(link.id)}' AND token_issued_at IS NULL RETURNING id`);
        if (!upd[0]) return json(req, { message: "Link this Pocket again in Companion.", code: "token_already_issued" }, 409);
        return json(req, { ok: true, device_token: token });
      }

      const pairMatch = path.match(/^\/v1\/pair\/sessions\/([^/]+)$/);
      if (req.method === "GET" && pairMatch) {
        const code = normalizePairCode(decodeURIComponent(pairMatch[1]));
        if (!isValidPairCode(code)) return json(req, { status: "expired" });
        const rows = await query(`SELECT id, device_id, status, expires_at, device_token_once FROM pair_sessions WHERE code_hash='${esc(sha256Hex(code))}'`);
        const row = rows[0];
        if (!row) return json(req, { status: "expired" });
        if (row.status === "claimed") {
          // Token-aware firmware (x-pocket-token: 1) collects its own token exactly once.
          const wants = (req.headers.get("x-pocket-token") || "") === "1";
          if (wants && row.device_token_once) {
            const now = new Date().toISOString();
            await query(`UPDATE pair_sessions SET device_token_once=NULL WHERE id='${esc(row.id)}'`);
            await query(`UPDATE device_links SET token_issued_at='${now}' WHERE device_id='${esc(row.device_id)}' AND device_token_hash='${esc(sha256Hex(row.device_token_once))}'`);
            return json(req, { status: "claimed", device_label: "Pocket", device_token: row.device_token_once });
          }
          return json(req, { status: "claimed", device_label: "Pocket" });
        }
        if (row.status !== "pending" || Date.parse(row.expires_at) < Date.now()) return json(req, { status: "expired" });
        return json(req, { status: "pending", device_label: "Pocket" });
      }

      if (req.method === "POST" && path === "/v1/pair/claim") {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const body = await req.json().catch(() => ({}));
        const code = normalizePairCode(typeof body.code === "string" ? body.code : "");
        if (!isValidPairCode(code)) return json(req, { message: "We couldn't find that code." }, 404);
        const rows = await query(`SELECT * FROM pair_sessions WHERE code_hash='${esc(sha256Hex(code))}'`);
        const row = rows[0];
        if (!row) return json(req, { message: "We couldn't find that code." }, 404);
        if (row.status === "claimed") return json(req, { message: "That code was already used." }, 400);
        if (row.status !== "pending" || Date.parse(row.expires_at) < Date.now()) {
          return json(req, { message: "This code has expired. Generate a new one on your Pocket." }, 410);
        }
        const existingRows = await query(`SELECT user_id, id FROM device_links WHERE device_id='${esc(row.device_id)}'`);
        const existing = existingRows[0];
        if (existing && existing.user_id !== user.id) {
          return json(req, { message: "This Pocket is linked to another account. Unlink it there first.", code: "pair_other_account" }, 409);
        }
        const now = new Date().toISOString();
        const deviceToken = randomToken(32);
        if (existing) {
          await query(`UPDATE device_links SET user_id='${esc(user.id)}', device_name='Pocket', linked_at='${now}', device_token_hash='${esc(sha256Hex(deviceToken))}', token_issued_at=NULL, last_seen_at='${now}' WHERE id='${esc(existing.id)}'`);
        } else {
          await query(`INSERT INTO device_links (id,user_id,device_id,device_name,linked_at,last_seen_at,device_token_hash) VALUES ('${esc(newId())}','${esc(user.id)}','${esc(row.device_id)}','Pocket','${now}','${now}','${esc(sha256Hex(deviceToken))}')`);
        }
        await query(`UPDATE pair_sessions SET status='claimed', claimed_at='${now}', claimed_by_user_id='${esc(user.id)}', device_token_once='${esc(deviceToken)}' WHERE id='${esc(row.id)}'`);
        // The device token goes only to the Pocket (pair poll), never to the phone.
        return json(req, { device_id: row.device_id, device_name: "Pocket", linked_at: now });
      }

      if (req.method === "GET" && path === "/v1/devices") {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const rows = await query(`SELECT id, device_id, device_name, linked_at, last_seen_at, home_order_json FROM device_links WHERE user_id='${esc(user.id)}' ORDER BY linked_at DESC`);
        const devices = rows.map(({ home_order_json, ...d }) => ({ ...d, home_order: readHomeOrder(home_order_json) }));
        return json(req, { devices });
      }

      const deviceIdMatch = path.match(/^\/v1\/devices\/([^/]+)$/);
      if (deviceIdMatch && (req.method === "GET" || req.method === "PATCH")) {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const id = decodeURIComponent(deviceIdMatch[1]);
        const rows = await query(
          `SELECT id, device_id, device_name, linked_at, last_seen_at, parental_json, lock_message, volume_percent, brightness_percent, home_order_json FROM device_links WHERE (id='${esc(id)}' OR device_id='${esc(id)}') AND user_id='${esc(user.id)}' LIMIT 1`,
        );
        const row = rows[0];
        if (!row) return json(req, { message: "That device wasn't found." }, 404);
        if (req.method === "GET") {
          let parental = {};
          try { parental = JSON.parse(row.parental_json || "{}") || {}; } catch { parental = {}; }
          return json(req, {
            id: row.id,
            device_id: row.device_id,
            device_name: row.device_name,
            linked_at: row.linked_at,
            last_seen_at: row.last_seen_at,
            parental,
            lock_message: row.lock_message || "",
            volume_percent: Number(row.volume_percent ?? 80),
            brightness_percent: Number(row.brightness_percent ?? 50),
            home_order: readHomeOrder(row.home_order_json),
          });
        }
        const body = await req.json().catch(() => ({}));
        const hasName = typeof body.device_name === "string";
        const hasLockMessage = typeof body.lock_message === "string";
        const hasVolume = Number.isFinite(body.volume_percent);
        const hasBrightness = Number.isFinite(body.brightness_percent);
        const hasHomeOrder = body.home_order !== undefined;
        if (!hasName && !hasLockMessage && !hasVolume && !hasBrightness && !hasHomeOrder) {
          return json(req, { message: "Nothing to update." }, 400);
        }
        const sets = [];
        let name = row.device_name;
        if (hasName) {
          name = body.device_name.trim();
          if (!name) return json(req, { message: "Enter a name" }, 400);
          if (name.length > 20) return json(req, { message: "Name must be 20 characters or fewer." }, 400);
          if (!/^[\p{L}\p{N} \-']+$/u.test(name)) {
            return json(req, { message: "That name uses characters that aren't allowed." }, 400);
          }
          sets.push(`device_name=${sqlStr(name)}`);
        }
        let lockMessage = row.lock_message || "";
        if (hasLockMessage) {
          lockMessage = body.lock_message.trim().slice(0, 40);
          sets.push(`lock_message=${sqlStr(lockMessage)}`);
        }
        let volumePercent = Number(row.volume_percent ?? 80);
        if (hasVolume) {
          volumePercent = Math.max(0, Math.min(100, Math.round(body.volume_percent)));
          sets.push(`volume_percent=${volumePercent}`);
        }
        let brightnessPercent = Number(row.brightness_percent ?? 50);
        if (hasBrightness) {
          brightnessPercent = Math.max(0, Math.min(100, Math.round(body.brightness_percent)));
          sets.push(`brightness_percent=${brightnessPercent}`);
        }
        let homeOrder = readHomeOrder(row.home_order_json);
        if (hasHomeOrder) {
          const parsed = parseHomeOrder(body.home_order);
          if (!parsed.ok) return json(req, { message: parsed.message }, 400);
          homeOrder = parsed.value;
          sets.push(`home_order_json=${sqlStr(JSON.stringify(homeOrder))}`);
        }
        if (sets.length) {
          await query(`UPDATE device_links SET ${sets.join(", ")} WHERE id='${esc(row.id)}'`);
        }
        return json(req, {
          id: row.id,
          device_id: row.device_id,
          device_name: name,
          linked_at: row.linked_at,
          last_seen_at: row.last_seen_at,
          lock_message: lockMessage,
          volume_percent: volumePercent,
          brightness_percent: brightnessPercent,
          home_order: homeOrder,
        });
      }

      // Find my Pocket: queue a chime, delivered once on the device's next attest (30 min TTL).
      const deviceRingMatch = path.match(/^\/v1\/devices\/([^/]+)\/ring$/);
      if (req.method === "POST" && deviceRingMatch) {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const id = decodeURIComponent(deviceRingMatch[1]);
        const rows = await query(
          `SELECT id FROM device_links WHERE (id='${esc(id)}' OR device_id='${esc(id)}') AND user_id='${esc(user.id)}' LIMIT 1`,
        );
        if (!rows[0]) return json(req, { message: "That device wasn't found." }, 404);
        const queuedAt = new Date().toISOString();
        await query(`UPDATE device_links SET ring_requested_at='${queuedAt}' WHERE id='${esc(rows[0].id)}'`);
        return json(req, { ok: true, queued_at: queuedAt });
      }

      // "Back up now": Pocket backs up on its next check-in (attest → backup_now, 24 h TTL).
      const deviceBackupReqMatch = path.match(/^\/v1\/devices\/([^/]+)\/backup-request$/);
      if (req.method === "POST" && deviceBackupReqMatch) {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const id = decodeURIComponent(deviceBackupReqMatch[1]);
        const rows = await query(
          `SELECT id FROM device_links WHERE (id=${sqlStr(id)} OR device_id=${sqlStr(id)}) AND user_id=${sqlStr(user.id)} LIMIT 1`,
        );
        if (!rows[0]) return json(req, { message: "That device wasn't found." }, 404);
        const queuedAt = new Date().toISOString();
        await query(`UPDATE device_links SET backup_requested_at=${sqlStr(queuedAt)} WHERE id=${sqlStr(rows[0].id)}`);
        return json(req, { ok: true, queued_at: queuedAt });
      }

      const deviceLinkMatch = path.match(/^\/v1\/devices\/([^/]+)\/link$/);
      if (req.method === "DELETE" && deviceLinkMatch) {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const id = decodeURIComponent(deviceLinkMatch[1]);
        const rows = await query(
          `SELECT id FROM device_links WHERE (id='${esc(id)}' OR device_id='${esc(id)}') AND user_id='${esc(user.id)}' LIMIT 1`,
        );
        if (!rows[0]) return json(req, { message: "That device wasn't found." }, 404);
        await query(`DELETE FROM device_links WHERE id='${esc(rows[0].id)}'`);
        return json(req, { ok: true });
      }

      const deviceWifiMatch = path.match(/^\/v1\/devices\/([^/]+)\/wifi$/);
      if (req.method === "POST" && deviceWifiMatch) {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const id = decodeURIComponent(deviceWifiMatch[1]);
        const rows = await query(
          `SELECT id FROM device_links WHERE (id='${esc(id)}' OR device_id='${esc(id)}') AND user_id='${esc(user.id)}' LIMIT 1`,
        );
        if (!rows[0]) return json(req, { message: "That device wasn't found." }, 404);
        const body = await req.json().catch(() => ({}));
        const ssid = typeof body.ssid === "string" ? body.ssid.trim().slice(0, 32) : "";
        const password = typeof body.password === "string" ? body.password.slice(0, 64) : "";
        if (!ssid) return json(req, { message: "Enter a network name." }, 400);
        const now = new Date().toISOString();
        await query(
          `UPDATE device_links SET pending_wifi_ssid=${sqlStr(ssid)}, pending_wifi_password=${sqlStr(password)}, pending_wifi_at='${now}' WHERE id='${esc(rows[0].id)}'`,
        );
        return json(req, { ok: true, queued: true });
      }

      const deviceParentalMatch = path.match(/^\/v1\/devices\/([^/]+)\/parental$/);
      if (deviceParentalMatch && (req.method === "GET" || req.method === "PATCH")) {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const id = decodeURIComponent(deviceParentalMatch[1]);
        const rows = await query(
          `SELECT id, parental_json FROM device_links WHERE (id='${esc(id)}' OR device_id='${esc(id)}') AND user_id='${esc(user.id)}' LIMIT 1`,
        );
        if (!rows[0]) return json(req, { message: "That device wasn't found." }, 404);
        if (req.method === "GET") {
          let parental = {};
          try { parental = JSON.parse(rows[0].parental_json || "{}") || {}; } catch { parental = {}; }
          return json(req, { parental });
        }
        const body = await req.json().catch(() => ({}));
        const pin_gated_apps = Array.isArray(body.pin_gated_apps)
          ? body.pin_gated_apps.map((a) => String(a).trim().toLowerCase()).filter(Boolean).slice(0, 16)
          : [];
        const hide_pass_share = body.hide_pass_share === true;
        const block_connectors = body.block_connectors === true;
        const parental = { pin_gated_apps, hide_pass_share, block_connectors };
        await query(
          `UPDATE device_links SET parental_json=${sqlStr(JSON.stringify(parental))} WHERE id='${esc(rows[0].id)}'`,
        );
        return json(req, { parental });
      }

      // Device pushes its own rotary-set volume/brightness so Companion mirrors what's on the
      // device (no PIN/session needed — device key + device_id only, like attest).
      if (req.method === "PATCH" && path === "/v1/device/settings") {
        const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
        const userId = await userIdForDeviceKey(req, deviceId);
        if (!userId) return json(req, { message: "Sign in to continue." }, 401);
        const body = await req.json().catch(() => ({}));
        const sets = [];
        if (Number.isFinite(body.volume_percent)) {
          sets.push(`volume_percent=${Math.max(0, Math.min(100, Math.round(body.volume_percent)))}`);
        }
        if (Number.isFinite(body.brightness_percent)) {
          sets.push(`brightness_percent=${Math.max(0, Math.min(100, Math.round(body.brightness_percent)))}`);
        }
        if (!sets.length) return json(req, { message: "Nothing to update." }, 400);
        await query(`UPDATE device_links SET ${sets.join(", ")} WHERE device_id='${esc(deviceId)}'`);
        return json(req, { ok: true });
      }

      // Device heartbeat + entitlement + pending Wi‑Fi / parental (x-device-key + device_id)
      if (req.method === "GET" && path === "/v1/device/attest") {
        const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
        const userId = await userIdForDeviceKey(req, deviceId);
        if (!userId) return json(req, { message: "Sign in to continue." }, 401);
        const now = new Date().toISOString();
        const links = await query(
          `SELECT id, device_id, device_name, pending_wifi_ssid, pending_wifi_password, pending_wifi_at, parental_json, lock_message, volume_percent, brightness_percent, sd_present, ring_requested_at, home_order_json, backup_requested_at
           FROM device_links WHERE device_id='${esc(deviceId)}' ORDER BY linked_at DESC LIMIT 1`,
        );
        const link = links[0];
        if (!link) return json(req, { message: "That device wasn't found." }, 404);
        // microSD report on the heartbeat: header x-pocket-sd: 1|0 or ?sd=1|0 (only when sent).
        const sdHeader = (req.headers.get("x-pocket-sd") || "").trim();
        const sdParam = (u.searchParams.get("sd") || "").trim();
        const sdRaw = sdHeader === "1" || sdHeader === "0" ? sdHeader : sdParam === "1" || sdParam === "0" ? sdParam : "";
        let sdPresent = pgBool(link.sd_present);
        if (sdRaw) {
          sdPresent = sdRaw === "1";
          await query(`UPDATE device_links SET last_seen_at='${now}', sd_present=${sdPresent ? "true" : "false"} WHERE id='${esc(link.id)}'`);
        } else {
          await query(`UPDATE device_links SET last_seen_at='${now}' WHERE id='${esc(link.id)}'`);
        }
        const subs = await query(`SELECT status, trial_ends_at, current_period_end FROM subscription_mirrors WHERE user_id='${esc(userId)}'`);
        const sub = subs[0];
        const entitled = sub?.status === "active" || sub?.status === "trialing";
        let parental = {};
        try { parental = JSON.parse(link.parental_json || "{}") || {}; } catch { parental = {}; }
        let pending_wifi = null;
        if (link.pending_wifi_ssid) {
          pending_wifi = {
            ssid: link.pending_wifi_ssid,
            password: link.pending_wifi_password || "",
            queued_at: link.pending_wifi_at || null,
          };
          await query(
            `UPDATE device_links SET pending_wifi_ssid=NULL, pending_wifi_password=NULL, pending_wifi_at=NULL WHERE id='${esc(link.id)}'`,
          );
        }
        // Find my Pocket: deliver a queued ring once (the conditional clear means only one
        // concurrent attest wins it); rings older than RING_TTL_MS expire silently.
        let ring = false;
        if (link.ring_requested_at) {
          const cleared = await query(
            `UPDATE device_links SET ring_requested_at=NULL WHERE id='${esc(link.id)}' AND ring_requested_at IS NOT NULL RETURNING id`,
          );
          const queuedMs = Date.parse(link.ring_requested_at);
          ring = cleared.length > 0 && Number.isFinite(queuedMs) && Date.now() - queuedMs <= RING_TTL_MS;
        }
        // "Back up now" from Companion: delivered once (conditional clear), expires after 24 h.
        let backupNow = false;
        if (link.backup_requested_at) {
          const cleared = await query(
            `UPDATE device_links SET backup_requested_at=NULL WHERE id='${esc(link.id)}' AND backup_requested_at IS NOT NULL RETURNING id`,
          );
          const queuedMs = Date.parse(link.backup_requested_at);
          backupNow = cleared.length > 0 && Number.isFinite(queuedMs) && Date.now() - queuedMs <= BACKUP_REQUEST_TTL_MS;
        }
        // Wallpapers: a cheap revision so the Pocket fetches the list only when it changes.
        let wallpapers_rev = null;
        if ((req.headers.get("x-device-token") || "").trim()) {
          const wl = (await wallpapersForUser(userId)).filter((r) => wallTargets(r, link.device_id)).slice(0, WALL_MAX_PER_DEVICE);
          wallpapers_rev = createHash("sha256").update(wl.map((r) => r.id + ":" + r.sha256 + ":" + r.layout_json).join("|")).digest("hex").slice(0, 16);
        }
        let apps_rev = null;
        if ((req.headers.get("x-device-token") || "").trim()) {
          apps_rev = appsRevFor(await appInstalls(userId), link.device_id, await registryApps());
        }
        return json(req, {
          wallpapers_rev,
          apps_rev,
          entitled,
          status: sub?.status || "free",
          trial_ends_at: sub?.trial_ends_at || null,
          current_period_end: sub?.current_period_end || null,
          cloud_entitled: entitled,
          device_id: link.device_id,
          device_name: link.device_name,
          lock_message: link.lock_message || "",
          volume_percent: Number(link.volume_percent ?? 80),
          brightness_percent: Number(link.brightness_percent ?? 50),
          parental,
          pin_gated_apps: Array.isArray(parental.pin_gated_apps) ? parental.pin_gated_apps : [],
          hide_pass_share: parental.hide_pass_share === true,
          block_connectors: parental.block_connectors === true,
          pending_wifi,
          sd_present: sdPresent,
          ring,
          backup_now: backupNow,
          home_order: readHomeOrder(link.home_order_json),
        });
      }

      // ---- Music (Companion manage + device pull) ----
      if (req.method === "GET" && path === "/v1/music") {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const rows = await query(
          `SELECT id, title, filename, mime, size_bytes, created_at FROM music_tracks WHERE user_id='${esc(gate.user.id)}' AND deleted_at IS NULL ORDER BY created_at DESC`,
        );
        return json(req, { tracks: rows.map(serializeMusicMeta) });
      }

      if (req.method === "POST" && path === "/v1/music") {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const body = await req.json().catch(() => ({}));
        const title = typeof body.title === "string" ? body.title.trim().slice(0, 120) : "";
        let filename = typeof body.filename === "string" ? body.filename.trim().slice(0, 180) : "";
        const mime = typeof body.mime === "string" ? body.mime.trim().slice(0, 80) : "audio/wav";
        const audioB64 = typeof body.audio_b64 === "string" ? body.audio_b64.replace(/\s+/g, "") : "";
        if (!audioB64) return json(req, { message: "Choose a WAV file to upload." }, 400);
        let decoded;
        try {
          decoded = Buffer.from(audioB64, "base64");
        } catch {
          return json(req, { message: "Upload data was invalid." }, 400);
        }
        if (!decoded.length || decoded.length > MUSIC_MAX_BYTES) {
          return json(
            req,
            {
              message: `Track must be a WAV under ${MUSIC_MAX_SD_BYTES / (1024 * 1024)} MB (or ${MUSIC_MAX_INTERNAL_BYTES / (1024 * 1024)} MB without an SD card).`,
            },
            400,
          );
        }
        if (decoded.length < 12 || decoded.toString("ascii", 0, 4) !== "RIFF" || decoded.toString("ascii", 8, 12) !== "WAVE") {
          return json(req, { message: "Only WAV audio is supported on Pocket." }, 400);
        }
        if (!filename) filename = "track.wav";
        if (!/\.wav$/i.test(filename)) filename = `${filename.replace(/\.[^.]+$/, "") || "track"}.wav`;
        filename = filename.replace(/[/\\]/g, "_");
        const id = newId();
        const now = new Date().toISOString();
        const safeTitle = title || filename.replace(/\.wav$/i, "");
        await query(
          `INSERT INTO music_tracks (id,user_id,title,filename,mime,size_bytes,audio_b64,created_at,deleted_at) VALUES ('${esc(id)}','${esc(gate.user.id)}',${sqlStr(safeTitle)},${sqlStr(filename)},${sqlStr(mime || "audio/wav")},${decoded.length},${sqlStr(audioB64)},'${now}',NULL)`,
        );
        const rows = await query(
          `SELECT id, title, filename, mime, size_bytes, created_at FROM music_tracks WHERE id='${esc(id)}'`,
        );
        return json(req, { track: serializeMusicMeta(rows[0]) }, 201);
      }

      const musicAudioMatch = path.match(/^\/v1\/music\/([^/]+)\/audio$/);
      if (req.method === "GET" && musicAudioMatch) {
        const id = decodeURIComponent(musicAudioMatch[1]);
        // Companion session OR device key + device_id
        let userId = null;
        const gate = await requireEntitledUser(req);
        if (!gate.error) {
          userId = gate.user.id;
        } else {
          const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
          userId = await userIdForDeviceKey(req, deviceId);
          if (!userId) return gate.error;
        }
        const rows = await query(
          `SELECT mime, audio_b64, filename FROM music_tracks WHERE id='${esc(id)}' AND user_id='${esc(userId)}' AND deleted_at IS NULL`,
        );
        if (!rows[0]) return json(req, { message: "Track not found." }, 404);
        const bytes = Buffer.from(String(rows[0].audio_b64 || ""), "base64");
        return new Response(bytes, {
          status: 200,
          headers: {
            ...cors(req),
            "content-type": rows[0].mime || "audio/wav",
            "content-length": String(bytes.length),
            "content-disposition": `attachment; filename="${String(rows[0].filename || "track.wav").replace(/"/g, "")}"`,
          },
        });
      }

      // "/v1/music/limits" is its own route further down — don't treat "limits" as a track id.
      const musicIdMatch = path === "/v1/music/limits" ? null : path.match(/^\/v1\/music\/([^/]+)$/);
      if (musicIdMatch) {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(musicIdMatch[1]);
        if (req.method === "DELETE") {
          const existing = await query(
            `SELECT id FROM music_tracks WHERE id='${esc(id)}' AND user_id='${esc(gate.user.id)}' AND deleted_at IS NULL`,
          );
          if (!existing[0]) return json(req, { message: "Track not found." }, 404);
          const now = new Date().toISOString();
          await query(`UPDATE music_tracks SET deleted_at='${now}' WHERE id='${esc(id)}'`);
          return new Response(null, { status: 204, headers: cors(req) });
        }
        if (req.method === "GET") {
          const rows = await query(
            `SELECT id, title, filename, mime, size_bytes, created_at FROM music_tracks WHERE id='${esc(id)}' AND user_id='${esc(gate.user.id)}' AND deleted_at IS NULL`,
          );
          if (!rows[0]) return json(req, { message: "Track not found." }, 404);
          return json(req, serializeMusicMeta(rows[0]));
        }
      }

      // Device library pull (x-device-key + device_id); reports microSD for Companion limits
      if (req.method === "GET" && path === "/v1/device/music") {
        const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
        const userId = await userIdForDeviceKey(req, deviceId);
        if (!userId) return json(req, { message: "Sign in to continue." }, 401);
        const sdHeader = (req.headers.get("x-pocket-sd") || "").trim();
        const sdQuery = u.searchParams.get("sd") === "1";
        const sdPresent = sdHeader === "1" || sdQuery;
        const now = new Date().toISOString();
        await query(
          `UPDATE device_links SET last_seen_at='${now}', sd_present=${sdPresent ? "true" : "false"} WHERE device_id='${esc(deviceId)}'`,
        );
        const rows = await query(
          `SELECT id, title, filename, mime, size_bytes, created_at FROM music_tracks WHERE user_id='${esc(userId)}' AND deleted_at IS NULL ORDER BY created_at DESC`,
        );
        return json(req, { tracks: rows.map(serializeMusicMeta), sd_present: sdPresent });
      }

      if (req.method === "GET" && path === "/v1/music/limits") {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const rows = await query(
          // Only devices seen in the last 30 days count — a Pocket unplugged long ago shouldn't keep limits raised.
          `SELECT COALESCE(BOOL_OR(sd_present), false) AS sd FROM device_links WHERE user_id='${esc(gate.user.id)}' AND last_seen_at IS NOT NULL AND last_seen_at > ${sqlStr(new Date(Date.now() - 30 * 86400000).toISOString())}`,
        );
        const sd = Boolean(rows[0]?.sd === true || rows[0]?.sd === "t" || rows[0]?.sd === "true");
        return json(req, {
          sd_present: sd,
          max_upload_bytes: sd ? MUSIC_MAX_SD_BYTES : MUSIC_MAX_INTERNAL_BYTES,
          internal_max_bytes: MUSIC_MAX_INTERNAL_BYTES,
          sd_max_bytes: MUSIC_MAX_SD_BYTES,
        });
      }

      // ---- Reading — eBooks (Companion manage + device pull) ----
      if (req.method === "GET" && path === "/v1/books") {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const rows = await query(
          `SELECT id, title, author, format, filename, size_bytes, created_at FROM books WHERE user_id='${esc(gate.user.id)}' AND deleted_at IS NULL ORDER BY created_at DESC`,
        );
        const progressRows = await query(
          `SELECT book_id, page, pages, percent, updated_at FROM reading_progress WHERE user_id='${esc(gate.user.id)}'`,
        );
        const progress = new Map(progressRows.map((p) => [p.book_id, p]));
        return json(req, {
          books: rows.map((r) => ({ ...serializeBookMeta(r), progress: serializeProgress(progress.get(r.id)) })),
        });
      }

      if (req.method === "POST" && path === "/v1/books") {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const body = await req.json().catch(() => ({}));
        const title = typeof body.title === "string" ? body.title.trim().slice(0, 160) : "";
        const author = typeof body.author === "string" ? body.author.trim().slice(0, 120) : "";
        let filename = typeof body.filename === "string" ? body.filename.trim().slice(0, 180) : "";
        const fileB64 = typeof body.file_b64 === "string" ? body.file_b64.replace(/\s+/g, "") : "";
        if (!fileB64) return json(req, { message: "Choose an EPUB or TXT file to upload." }, 400);
        let decoded;
        try {
          decoded = Buffer.from(fileB64, "base64");
        } catch {
          return json(req, { message: "Upload data was invalid." }, 400);
        }
        if (!decoded.length || decoded.length > BOOK_MAX_BYTES) {
          return json(req, { message: `Books must be under ${BOOK_MAX_BYTES / (1024 * 1024)} MB.` }, 400);
        }
        const isEpub =
          decoded.length > 4 && decoded[0] === 0x50 && decoded[1] === 0x4b; // "PK" ZIP signature
        const declaredFormat = typeof body.format === "string" ? body.format.toLowerCase() : "";
        const format = isEpub || declaredFormat === "epub" ? "epub" : "txt";

        let text = "";
        if (format === "epub") {
          text = extractEpubText(decoded);
          if (!text) {
            return json(req, { message: "Couldn't read that EPUB — try exporting as plain text." }, 400);
          }
        } else {
          text = decoded.toString("utf8");
          if (!text.trim()) return json(req, { message: "That file looked empty." }, 400);
        }
        if (Buffer.byteLength(text, "utf8") > BOOK_MAX_TEXT_BYTES) {
          text = Buffer.from(text, "utf8").subarray(0, BOOK_MAX_TEXT_BYTES).toString("utf8");
        }
        if (!filename) filename = "book.txt";
        filename = filename.replace(/\.[^./\\]+$/, "").replace(/[/\\]/g, "_") + ".txt";
        const id = newId();
        const now = new Date().toISOString();
        const safeTitle = title || filename.replace(/\.txt$/i, "");
        const textB64 = Buffer.from(text, "utf8").toString("base64");
        await query(
          `INSERT INTO books (id,user_id,title,author,format,filename,size_bytes,text_b64,created_at,deleted_at) VALUES ('${esc(id)}','${esc(gate.user.id)}',${sqlStr(safeTitle)},${sqlStr(author)},${sqlStr(format)},${sqlStr(filename)},${Buffer.byteLength(text, "utf8")},${sqlStr(textB64)},'${now}',NULL)`,
        );
        const rows = await query(
          `SELECT id, title, author, format, filename, size_bytes, created_at FROM books WHERE id='${esc(id)}'`,
        );
        return json(req, { book: serializeBookMeta(rows[0]) }, 201);
      }

      const bookTextMatch = path.match(/^\/v1\/books\/([^/]+)\/text$/);
      if (req.method === "GET" && bookTextMatch) {
        const id = decodeURIComponent(bookTextMatch[1]);
        let userId = null;
        const gate = await requireEntitledUser(req);
        if (!gate.error) {
          userId = gate.user.id;
        } else {
          const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
          userId = await userIdForDeviceKey(req, deviceId);
          if (!userId) return gate.error;
        }
        const rows = await query(
          `SELECT text_b64, filename FROM books WHERE id='${esc(id)}' AND user_id='${esc(userId)}' AND deleted_at IS NULL`,
        );
        if (!rows[0]) return json(req, { message: "Book not found." }, 404);
        const bytes = Buffer.from(String(rows[0].text_b64 || ""), "base64");
        return new Response(bytes, {
          status: 200,
          headers: {
            ...cors(req),
            "content-type": "text/plain; charset=utf-8",
            "content-length": String(bytes.length),
            "content-disposition": `attachment; filename="${String(rows[0].filename || "book.txt").replace(/"/g, "")}"`,
          },
        });
      }

      const bookIdMatch = path.match(/^\/v1\/books\/([^/]+)$/);
      if (bookIdMatch) {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(bookIdMatch[1]);
        if (req.method === "DELETE") {
          const existing = await query(
            `SELECT id FROM books WHERE id='${esc(id)}' AND user_id='${esc(gate.user.id)}' AND deleted_at IS NULL`,
          );
          if (!existing[0]) return json(req, { message: "Book not found." }, 404);
          const now = new Date().toISOString();
          await query(`UPDATE books SET deleted_at='${now}' WHERE id='${esc(id)}'`);
          return new Response(null, { status: 204, headers: cors(req) });
        }
        if (req.method === "GET") {
          const rows = await query(
            `SELECT id, title, author, format, filename, size_bytes, created_at FROM books WHERE id='${esc(id)}' AND user_id='${esc(gate.user.id)}' AND deleted_at IS NULL`,
          );
          if (!rows[0]) return json(req, { message: "Book not found." }, 404);
          return json(req, serializeBookMeta(rows[0]));
        }
      }

      // Device library pull (x-device-key + device_id)
      if (req.method === "GET" && path === "/v1/device/books") {
        const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
        const userId = await userIdForDeviceKey(req, deviceId);
        if (!userId) return json(req, { message: "Sign in to continue." }, 401);
        const now = new Date().toISOString();
        await query(`UPDATE device_links SET last_seen_at='${now}' WHERE device_id='${esc(deviceId)}'`);
        const rows = await query(
          `SELECT id, title, author, format, filename, size_bytes, created_at FROM books WHERE user_id='${esc(userId)}' AND deleted_at IS NULL ORDER BY created_at DESC`,
        );
        return json(req, { books: rows.map(serializeBookMeta) });
      }

      // Device reading progress (x-device-key + device_id). One row per user+book; the latest
      // report wins. Unknown book ids are stored anyway — /v1/books only shows known books.
      if (req.method === "POST" && path === "/v1/device/reading") {
        const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
        const userId = await userIdForDeviceKey(req, deviceId);
        if (!userId) return json(req, { message: "Sign in to continue." }, 401);
        const body = await req.json().catch(() => null);
        const parsed = parseReadingProgress(body);
        if (!parsed.ok) return json(req, { message: parsed.message }, 400);
        const p = parsed.value;
        const now = new Date().toISOString();
        await query(
          `INSERT INTO reading_progress (user_id,book_id,device_id,page,pages,percent,updated_at) VALUES (${sqlStr(userId)},${sqlStr(p.book_id)},${sqlStr(deviceId)},${p.page},${p.pages},${p.percent},'${now}')
           ON CONFLICT (user_id, book_id) DO UPDATE SET device_id=EXCLUDED.device_id, page=EXCLUDED.page, pages=EXCLUDED.pages, percent=EXCLUDED.percent, updated_at=EXCLUDED.updated_at`,
        );
        await query(`UPDATE device_links SET last_seen_at='${now}' WHERE device_id=${sqlStr(deviceId)}`);
        return json(req, { ok: true });
      }

      // Device backup (x-device-key + device_id): settings, alarms and reading places, ≤ 64 KB.
      // Keeps the last 5 per user; "latest" spans every Pocket on the account (new-Pocket restore).
      if (req.method === "POST" && path === "/v1/device/backup") {
        const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
        const userId = await userIdForDeviceKey(req, deviceId);
        if (!userId) return json(req, { message: "Sign in to continue." }, 401);
        const raw = await req.text().catch(() => "");
        if (Buffer.byteLength(raw, "utf8") > DEVICE_BACKUP_MAX_BYTES) {
          return json(req, { message: "That backup is too large.", code: "backup_too_large" }, 413);
        }
        let body = null;
        try { body = JSON.parse(raw); } catch { body = null; }
        const parsed = parseDeviceBackup(body);
        if (!parsed.ok) return json(req, { message: parsed.message }, 400);
        const payload = JSON.stringify(parsed.value);
        const links = await query(
          `SELECT device_name FROM device_links WHERE device_id=${sqlStr(deviceId)} ORDER BY linked_at DESC LIMIT 1`,
        );
        const id = newId();
        const createdAt = new Date().toISOString();
        await query(
          `INSERT INTO device_backups (id,user_id,device_id,device_name,created_at,payload_json,size_bytes) VALUES (${sqlStr(id)},${sqlStr(userId)},${sqlStr(deviceId)},${sqlStr(links[0]?.device_name || "Pocket")},${sqlStr(createdAt)},${sqlStr(payload)},${Buffer.byteLength(payload, "utf8")})`,
        );
        await query(
          `DELETE FROM device_backups WHERE user_id=${sqlStr(userId)} AND id NOT IN (SELECT id FROM device_backups WHERE user_id=${sqlStr(userId)} ORDER BY created_at DESC, id DESC LIMIT ${DEVICE_BACKUP_KEEP})`,
        );
        await query(`UPDATE device_links SET last_seen_at=${sqlStr(createdAt)} WHERE device_id=${sqlStr(deviceId)}`);
        return json(req, { ok: true, backup_id: id, created_at: createdAt });
      }

      if (req.method === "GET" && path === "/v1/device/backup/latest") {
        const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
        const userId = await userIdForDeviceKey(req, deviceId);
        if (!userId) return json(req, { message: "Sign in to continue." }, 401);
        const rows = await query(
          `SELECT id, created_at, device_id, device_name, payload_json FROM device_backups WHERE user_id=${sqlStr(userId)} ORDER BY created_at DESC, id DESC LIMIT 1`,
        );
        if (!rows[0]) return json(req, { message: "There's no backup yet." }, 404);
        const p = readBackupPayload(rows[0].payload_json);
        return json(req, {
          backup: {
            id: rows[0].id,
            created_at: isoTs(rows[0].created_at),
            device_id: rows[0].device_id,
            device_name: rows[0].device_name,
            settings: p.settings,
            alarms: p.alarms,
            reading: p.reading,
          },
        });
      }

      // Calendar on Today (x-device-key + device_id): the user's ICS feeds, next 7 days, ≤ 30 events.
      if (req.method === "GET" && path === "/v1/device/calendar") {
        const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
        const userId = await userIdForDeviceKey(req, deviceId);
        if (!userId) return json(req, { message: "Sign in to continue." }, 401);
        const feeds = await query(
          `SELECT label, url FROM calendar_feeds WHERE user_id=${sqlStr(userId)} ORDER BY created_at ASC`,
        );
        const now = new Date();
        const events = await upcomingCalendarEvents(feeds, now);
        return json(req, { events, fetched_at: now.toISOString() });
      }

      // Device ↔ cloud notes/lists sync (x-device-key + device_id). Body carries only records
      // changed on the device since its last good sync (tombstones have deleted_at); the response
      // is the user's full live set plus tombstones from the last 30 days.
      if (req.method === "POST" && path === "/v1/device/sync") {
        const deviceId = u.searchParams.get("device_id") || req.headers.get("x-device-id") || "";
        const userId = await userIdForDeviceKey(req, deviceId);
        if (!userId) return json(req, { message: "Sign in to continue." }, 401);
        const body = await req.json().catch(() => null);
        if (!body || typeof body !== "object" || Array.isArray(body)) {
          return json(req, { message: "That sync request couldn't be read." }, 400);
        }
        const rawNotes = body.notes == null ? [] : body.notes;
        const rawLists = body.lists == null ? [] : body.lists;
        if (!Array.isArray(rawNotes) || !Array.isArray(rawLists)) {
          return json(req, { message: "That sync request couldn't be read." }, 400);
        }
        if (rawNotes.length > SYNC_MAX_RECORDS || rawLists.length > SYNC_MAX_RECORDS) {
          return json(
            req,
            { message: "Too many changes in one sync. Send them in smaller batches.", max_records: SYNC_MAX_RECORDS },
            413,
          );
        }
        const serverNow = new Date();
        const nowIso = serverNow.toISOString();
        const sdSet = typeof body.sd_present === "boolean" ? `, sd_present=${body.sd_present ? "true" : "false"}` : "";
        await query(`UPDATE device_links SET last_seen_at=${sqlStr(nowIso)}${sdSet} WHERE device_id=${sqlStr(deviceId)}`);

        const subs = await query(`SELECT status FROM subscription_mirrors WHERE user_id=${sqlStr(userId)}`);
        const st = subs[0]?.status;
        if (st !== "active" && st !== "trialing") {
          return json(req, { message: "Pocket Cloud is needed to sync notes and lists.", entitled: false }, 402);
        }

        const skipped = { notes: [], lists: [] };
        const noteRecs = [];
        for (const raw of rawNotes) {
          const r = syncNormalizeNote(raw, serverNow, deviceId);
          if (r.rec) noteRecs.push(r.rec);
          else skipped.notes.push(r.skip);
        }
        const listRecs = [];
        for (const raw of rawLists) {
          const r = syncNormalizeList(raw, serverNow, deviceId);
          if (r.rec) listRecs.push(r.rec);
          else skipped.lists.push(r.skip);
        }
        await applySyncRecords("cloud_notes", noteRecs, userId, skipped.notes);
        await applySyncRecords("cloud_lists", listRecs, userId, skipped.lists);

        const cutoff = sqlStr(new Date(serverNow.getTime() - SYNC_TOMBSTONE_DAYS * 86400000).toISOString());
        const notes = await query(
          `SELECT * FROM cloud_notes WHERE user_id=${sqlStr(userId)} AND (deleted_at IS NULL OR deleted_at > ${cutoff}) ORDER BY updated_at DESC`,
        );
        // Own lists plus lists shared into the user's Pocket Family (those carry the owner's name).
        const lists = await query(
          `SELECT l.*, u.email AS owner_email FROM cloud_lists l JOIN users u ON u.id=l.user_id WHERE ${listAccessSql(userId)} AND (l.deleted_at IS NULL OR l.deleted_at > ${cutoff}) ORDER BY l.updated_at DESC`,
        );
        const links = await query(
          `SELECT sd_present FROM device_links WHERE device_id=${sqlStr(deviceId)} ORDER BY linked_at DESC LIMIT 1`,
        );
        return json(req, {
          server_time: nowIso,
          sd_present: pgBool(links[0]?.sd_present),
          notes: notes.map((row) => syncIsoTimestamps(serializeNote(row))),
          lists: lists.map((row) =>
            row.family_id
              ? { ...syncIsoTimestamps(serializeList(row)), shared: true, owner: displayName(row.owner_email) }
              : { ...syncIsoTimestamps(serializeList(row)), shared: false },
          ),
          skipped,
        });
      }

      // Device OTA discovery — points at firmware-latest app image (A/B OTA, not merged USB bin).
      // Release asset first (always current), then the northwork.app mirror.
      if (req.method === "GET" && path === "/v1/firmware/latest") {
        const manifestUrls = [
          "https://github.com/NorthworkTech/Pocket/releases/download/firmware-latest/firmware-manifest.json",
          "https://northwork.app/pocket/firmware-manifest.json",
        ];
        for (const manifestUrl of manifestUrls) {
          try {
            const r = await fetch(manifestUrl, {
              headers: { Accept: "application/json" },
              redirect: "follow",
            });
            if (r.ok) {
              const body = await r.json();
              if (body && body.url) {
                return json(req, { ok: true, ...body }, 200, {
                  "cache-control": "public, max-age=60",
                });
              }
            }
          } catch (_) {
            /* try next */
          }
        }
        return json(req, {
          ok: true,
          build_id: "POCKET-LIVE-unknown",
          version: "firmware-latest",
          url: "https://github.com/NorthworkTech/Pocket/releases/download/firmware-latest/pocket.bin",
          size: 0,
          channel: "stable",
        }, 200, { "cache-control": "public, max-age=30" });
      }

      // Device dictation — raw PCM s16le → Whisper / Gemini multimodal.
      if (req.method === "GET" && path === "/v1/stt/status") {
        const key = (req.headers.get("x-device-key") || "").trim();
        if (!safeEqual(key, DEVICE_API_KEY)) return json(req, { message: "Unauthorized." }, 401);
        const secrets = await loadSttSecrets();
        return json(req, {
          ok: true,
          build: "scram-api-v25-guide",
          providers: {
            openai: Boolean(secrets.openai),
            groq: Boolean(secrets.groq),
            stt_proxy: Boolean(secrets.proxy),
            cloudflare_ai: Boolean(
              (process.env.CLOUDFLARE_ACCOUNT_ID || "").trim() &&
                (process.env.CLOUDFLARE_API_TOKEN || "").trim(),
            ),
            neon_ai_gateway: Boolean(
              (process.env.NEON_AI_GATEWAY_TOKEN || "").trim() &&
                (process.env.NEON_AI_GATEWAY_BASE_URL || "").trim(),
            ),
          },
        });
      }

      if (req.method === "POST" && path === "/v1/stt") {
        const key = (req.headers.get("x-device-key") || "").trim();
        if (!safeEqual(key, DEVICE_API_KEY)) return json(req, { message: "Unauthorized." }, 401);
        const rate = Math.max(8000, Math.min(48000, parseInt(req.headers.get("x-pcm-rate") || "16000", 10) || 16000));
        const encoding = (req.headers.get("x-pcm-encoding") || "s16le").toLowerCase();
        if (encoding !== "s16le") return json(req, { message: "Unsupported encoding." }, 400);
        const ab = await req.arrayBuffer();
        const pcm = Buffer.from(ab);
        if (pcm.length < 320) return json(req, { message: "Audio too short." }, 400);
        if (pcm.length > 2 * 1024 * 1024) return json(req, { message: "Audio too large." }, 413);
        try {
          const text = await transcribePcmS16le(pcm, rate);
          if (!text) return json(req, { message: "Couldn't hear anything.", text: "" }, 200);
          return json(req, { text });
        } catch (e) {
          return json(req, { message: "Couldn't reach speech service.", detail: String(e && e.message || e) }, 502);
        }
      }

      // ---- Pocket Family (session) ----
      // GET/POST/PATCH/DELETE /v1/family, POST /v1/family/code|join|leave, DELETE /v1/family/members/:id
      if (path === "/v1/family" || path.startsWith("/v1/family/")) {
        const gate = await requireSessionUser(req);
        if (gate.error) return gate.error;
        const me = gate.user.id;
        const notOwner = () => json(req, { message: "Only the family's owner can do that.", code: "not_family_owner" }, 403);
        const noFamily = () => json(req, { message: "You're not in a Pocket Family.", code: "not_found" }, 404);
        const unshareListsOf = (userId, familyId) =>
          query(`UPDATE cloud_lists SET family_id=NULL WHERE user_id=${sqlStr(userId)} AND family_id=${sqlStr(familyId)}`);

        if (path === "/v1/family" && req.method === "GET") {
          const fam = await myFamily(me);
          return json(req, { family: fam ? await serializeFamily(fam, me) : null });
        }
        if (path === "/v1/family" && req.method === "POST") {
          const body = await req.json().catch(() => ({}));
          const parsed = parseFamilyName(body?.name);
          if (!parsed.ok) return json(req, { message: parsed.message }, 400);
          if (await myFamily(me)) {
            return json(req, { message: "You're already in a Pocket Family.", code: "family_exists" }, 409);
          }
          const code = await freshFamilyCode();
          if (!code) return json(req, { message: "Couldn't make a code. Try again." }, 409);
          const id = newId();
          const now = new Date().toISOString();
          await query(
            `INSERT INTO families (id,name,owner_user_id,join_code,join_code_expires_at,created_at) VALUES (${sqlStr(id)},${sqlStr(parsed.value)},${sqlStr(me)},${sqlStr(code.code)},${sqlStr(code.expires_at)},${sqlStr(now)})`,
          );
          await query(
            `INSERT INTO family_members (user_id,family_id,role,joined_at) VALUES (${sqlStr(me)},${sqlStr(id)},'owner',${sqlStr(now)})`,
          );
          return json(req, { family: await serializeFamily(await myFamily(me), me) }, 201);
        }
        if (path === "/v1/family" && (req.method === "PATCH" || req.method === "DELETE")) {
          const fam = await myFamily(me);
          if (!fam) return noFamily();
          if (fam.owner_user_id !== me) return notOwner();
          if (req.method === "DELETE") {
            await query(`UPDATE cloud_lists SET family_id=NULL WHERE family_id=${sqlStr(fam.id)}`);
            await query(`DELETE FROM family_members WHERE family_id=${sqlStr(fam.id)}`);
            await query(`DELETE FROM families WHERE id=${sqlStr(fam.id)}`);
            return json(req, { ok: true });
          }
          const body = await req.json().catch(() => ({}));
          const parsed = parseFamilyName(body?.name);
          if (!parsed.ok) return json(req, { message: parsed.message }, 400);
          await query(`UPDATE families SET name=${sqlStr(parsed.value)} WHERE id=${sqlStr(fam.id)}`);
          return json(req, { family: await serializeFamily(await myFamily(me), me) });
        }
        if (path === "/v1/family/code" && req.method === "POST") {
          const fam = await myFamily(me);
          if (!fam) return noFamily();
          if (fam.owner_user_id !== me) return notOwner();
          const code = await freshFamilyCode();
          if (!code) return json(req, { message: "Couldn't make a code. Try again." }, 409);
          await query(
            `UPDATE families SET join_code=${sqlStr(code.code)}, join_code_expires_at=${sqlStr(code.expires_at)} WHERE id=${sqlStr(fam.id)}`,
          );
          return json(req, { family: await serializeFamily(await myFamily(me), me) });
        }
        if (path === "/v1/family/join" && req.method === "POST") {
          const body = await req.json().catch(() => ({}));
          const raw = typeof body?.code === "string" ? body.code : "";
          const invalid = () => json(req, { message: "We couldn't find that code.", code: "family_code_invalid" }, 404);
          if (!isValidPairCode(raw)) return invalid();
          const fams = await query(`SELECT * FROM families WHERE join_code=${sqlStr(normalizePairCode(raw))}`);
          const fam = fams[0];
          if (!fam) return invalid();
          const expMs = fam.join_code_expires_at ? Date.parse(fam.join_code_expires_at) : NaN;
          if (!Number.isFinite(expMs) || expMs <= Date.now()) {
            return json(req, { message: "This code has expired. Ask for a new one.", code: "family_code_expired" }, 410);
          }
          const current = await myFamily(me);
          if (current) {
            if (current.id === fam.id) return json(req, { family: await serializeFamily(current, me) });
            return json(req, { message: "You're already in a Pocket Family. Leave it first.", code: "family_exists" }, 409);
          }
          const count = await query(`SELECT COUNT(*) AS n FROM family_members WHERE family_id=${sqlStr(fam.id)}`);
          if (Number(count[0]?.n || 0) >= FAMILY_MAX_MEMBERS) {
            return json(req, { message: `A Pocket Family can have up to ${FAMILY_MAX_MEMBERS} people.`, code: "family_full" }, 409);
          }
          await query(
            `INSERT INTO family_members (user_id,family_id,role,joined_at) VALUES (${sqlStr(me)},${sqlStr(fam.id)},'member',${sqlStr(new Date().toISOString())}) ON CONFLICT (user_id) DO NOTHING`,
          );
          return json(req, { family: await serializeFamily(fam, me) });
        }
        if (path === "/v1/family/leave" && req.method === "POST") {
          const fam = await myFamily(me);
          if (!fam) return noFamily();
          if (fam.owner_user_id === me) {
            return json(req, { message: "You own this family. Delete it instead, or remove people first." }, 400);
          }
          await unshareListsOf(me, fam.id);
          await query(`DELETE FROM family_members WHERE user_id=${sqlStr(me)}`);
          return json(req, { ok: true });
        }
        const memberMatch = path.match(/^\/v1\/family\/members\/([^/]+)$/);
        if (memberMatch && req.method === "DELETE") {
          const fam = await myFamily(me);
          if (!fam) return noFamily();
          if (fam.owner_user_id !== me) return notOwner();
          const target = decodeURIComponent(memberMatch[1]);
          if (target === me) return json(req, { message: "You own this family. Delete it instead." }, 400);
          const member = await query(
            `SELECT user_id FROM family_members WHERE user_id=${sqlStr(target)} AND family_id=${sqlStr(fam.id)}`,
          );
          if (!member[0]) return json(req, { message: "That person isn't in your family." }, 404);
          await unshareListsOf(target, fam.id);
          await query(`DELETE FROM family_members WHERE user_id=${sqlStr(target)}`);
          return json(req, { family: await serializeFamily(fam, me) });
        }
        return json(req, { message: "Not found." }, 404);
      }

      // ---- Calendar feeds for Today (session) ----
      if (path === "/v1/calendar/feeds" && (req.method === "GET" || req.method === "POST")) {
        const gate = await requireSessionUser(req);
        if (gate.error) return gate.error;
        const me = gate.user.id;
        if (req.method === "GET") {
          const rows = await query(`SELECT * FROM calendar_feeds WHERE user_id=${sqlStr(me)} ORDER BY created_at ASC`);
          return json(req, { feeds: rows.map(serializeFeed), max_feeds: CALENDAR_MAX_FEEDS });
        }
        const body = await req.json().catch(() => ({}));
        const url = normalizeFeedUrl(body?.url);
        if (!url.ok) return json(req, { message: url.message }, 400);
        const count = await query(`SELECT COUNT(*) AS n FROM calendar_feeds WHERE user_id=${sqlStr(me)}`);
        if (Number(count[0]?.n || 0) >= CALENDAR_MAX_FEEDS) {
          return json(req, { message: `You can add up to ${CALENDAR_MAX_FEEDS} calendars.`, code: "calendar_limit" }, 409);
        }
        const dup = await query(`SELECT id FROM calendar_feeds WHERE user_id=${sqlStr(me)} AND url=${sqlStr(url.value)}`);
        if (dup[0]) return json(req, { message: "That calendar is already added.", code: "conflict" }, 409);
        const feed = { id: newId(), label: normalizeFeedLabel(body?.label), url: url.value, created_at: new Date().toISOString() };
        await query(
          `INSERT INTO calendar_feeds (id,user_id,label,url,created_at) VALUES (${sqlStr(feed.id)},${sqlStr(me)},${sqlStr(feed.label)},${sqlStr(feed.url)},${sqlStr(feed.created_at)})`,
        );
        return json(req, { feed: serializeFeed(feed) }, 201);
      }
      const calFeedMatch = path.match(/^\/v1\/calendar\/feeds\/([^/]+)$/);
      if (calFeedMatch && req.method === "DELETE") {
        const gate = await requireSessionUser(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(calFeedMatch[1]);
        const rows = await query(
          `DELETE FROM calendar_feeds WHERE id=${sqlStr(id)} AND user_id=${sqlStr(gate.user.id)} RETURNING id`,
        );
        if (!rows[0]) return json(req, { message: "That calendar wasn't found." }, 404);
        return json(req, { ok: true });
      }

      // ---- Notes ----
      if (req.method === "GET" && path === "/v1/notes") {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const includeDeleted = u.searchParams.get("include_deleted") === "1";
        const rows = await query(
          includeDeleted
            ? `SELECT * FROM cloud_notes WHERE user_id='${esc(gate.user.id)}' ORDER BY updated_at DESC`
            : `SELECT * FROM cloud_notes WHERE user_id='${esc(gate.user.id)}' AND deleted_at IS NULL ORDER BY updated_at DESC`,
        );
        return json(req, { notes: rows.map(serializeNote) });
      }

      if (req.method === "POST" && path === "/v1/notes") {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const body = await req.json().catch(() => ({}));
        const now = new Date().toISOString();
        const id = typeof body.id === "string" && body.id ? body.id : newId();
        const title = typeof body.title === "string" ? body.title : "";
        const noteBody = typeof body.body === "string" ? body.body : "";
        const updated_at = typeof body.updated_at === "string" ? body.updated_at : now;
        const updated_by =
          typeof body.updated_by_device_id === "string" && body.updated_by_device_id
            ? body.updated_by_device_id
            : "pwa";
        const created_at = typeof body.created_at === "string" ? body.created_at : now;
        const deleted_at =
          body.deleted_at === null
            ? null
            : typeof body.deleted_at === "string"
              ? body.deleted_at
              : null;
        const existing = await query(`SELECT * FROM cloud_notes WHERE id='${esc(id)}'`);
        if (existing[0]) {
          if (existing[0].user_id !== gate.user.id) return json(req, { message: "Forbidden." }, 403);
          await query(
            `UPDATE cloud_notes SET title=${sqlStr(title)}, body=${sqlStr(noteBody)}, updated_at=${sqlStr(updated_at)}, updated_by_device_id=${sqlStr(updated_by)}, deleted_at=${sqlNullable(deleted_at)}, version=version+1 WHERE id='${esc(id)}'`,
          );
        } else {
          await query(
            `INSERT INTO cloud_notes (id,user_id,title,body,created_at,updated_at,updated_by_device_id,deleted_at,version) VALUES ('${esc(id)}','${esc(gate.user.id)}',${sqlStr(title)},${sqlStr(noteBody)},${sqlStr(created_at)},${sqlStr(updated_at)},${sqlStr(updated_by)},${sqlNullable(deleted_at)},1)`,
          );
        }
        const rows = await query(`SELECT * FROM cloud_notes WHERE id='${esc(id)}'`);
        return json(req, { note: serializeNote(rows[0]) }, existing[0] ? 200 : 201);
      }

      const noteMatch = path.match(/^\/v1\/notes\/([^/]+)$/);
      if (noteMatch) {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(noteMatch[1]);
        if (req.method === "GET") {
          const rows = await query(
            `SELECT * FROM cloud_notes WHERE id='${esc(id)}' AND user_id='${esc(gate.user.id)}'`,
          );
          if (!rows[0]) return json(req, { message: "Note not found." }, 404);
          return json(req, serializeNote(rows[0]));
        }
        if (req.method === "PATCH") {
          const body = await req.json().catch(() => ({}));
          const existing = await query(
            `SELECT * FROM cloud_notes WHERE id='${esc(id)}' AND user_id='${esc(gate.user.id)}'`,
          );
          if (!existing[0]) return json(req, { message: "Note not found." }, 404);
          const title = typeof body.title === "string" ? body.title : existing[0].title;
          const noteBody = typeof body.body === "string" ? body.body : existing[0].body;
          const updated_at = typeof body.updated_at === "string" ? body.updated_at : new Date().toISOString();
          const updated_by =
            typeof body.updated_by_device_id === "string" && body.updated_by_device_id
              ? body.updated_by_device_id
              : "pwa";
          await query(
            `UPDATE cloud_notes SET title=${sqlStr(title)}, body=${sqlStr(noteBody)}, updated_at=${sqlStr(updated_at)}, updated_by_device_id=${sqlStr(updated_by)}, version=version+1 WHERE id='${esc(id)}'`,
          );
          const rows = await query(`SELECT * FROM cloud_notes WHERE id='${esc(id)}'`);
          return json(req, serializeNote(rows[0]));
        }
        if (req.method === "DELETE") {
          const body = await req.json().catch(() => ({}));
          const existing = await query(
            `SELECT * FROM cloud_notes WHERE id='${esc(id)}' AND user_id='${esc(gate.user.id)}'`,
          );
          if (!existing[0]) return json(req, { message: "Note not found." }, 404);
          const updated_at = typeof body.updated_at === "string" ? body.updated_at : new Date().toISOString();
          const updated_by =
            typeof body.updated_by_device_id === "string" && body.updated_by_device_id
              ? body.updated_by_device_id
              : "pwa";
          await query(
            `UPDATE cloud_notes SET deleted_at=${sqlStr(updated_at)}, updated_at=${sqlStr(updated_at)}, updated_by_device_id=${sqlStr(updated_by)}, version=version+1 WHERE id='${esc(id)}'`,
          );
          return new Response(null, { status: 204, headers: cors(req) });
        }
      }

      // ---- Lists ----
      if (req.method === "GET" && path === "/v1/lists") {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const includeDeleted = u.searchParams.get("include_deleted") === "1";
        // Own lists plus lists shared into the user's Pocket Family.
        const rows = await query(
          `SELECT l.*, u.email AS owner_email FROM cloud_lists l JOIN users u ON u.id=l.user_id WHERE ${listAccessSql(gate.user.id)}${includeDeleted ? "" : " AND l.deleted_at IS NULL"} ORDER BY l.updated_at DESC`,
        );
        return json(req, { lists: rows.map((r) => serializeListFor(r, gate.user.id)) });
      }

      if (req.method === "POST" && path === "/v1/lists") {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const body = await req.json().catch(() => ({}));
        const now = new Date().toISOString();
        const id = typeof body.id === "string" && body.id ? body.id : newId();
        const title = typeof body.title === "string" ? body.title : "";
        const itemsJson = JSON.stringify(Array.isArray(body.items) ? body.items : []);
        const updated_at = typeof body.updated_at === "string" ? body.updated_at : now;
        const updated_by =
          typeof body.updated_by_device_id === "string" && body.updated_by_device_id
            ? body.updated_by_device_id
            : "pwa";
        const created_at = typeof body.created_at === "string" ? body.created_at : now;
        const existing = await query(`SELECT * FROM cloud_lists WHERE id='${esc(id)}'`);
        if (existing[0]) {
          if (existing[0].user_id !== gate.user.id) return json(req, { message: "Forbidden." }, 403);
          await query(
            `UPDATE cloud_lists SET title=${sqlStr(title)}, items_json=${sqlStr(itemsJson)}, updated_at=${sqlStr(updated_at)}, updated_by_device_id=${sqlStr(updated_by)}, version=version+1 WHERE id='${esc(id)}'`,
          );
        } else {
          await query(
            `INSERT INTO cloud_lists (id,user_id,title,items_json,created_at,updated_at,updated_by_device_id,deleted_at,version) VALUES ('${esc(id)}','${esc(gate.user.id)}',${sqlStr(title)},${sqlStr(itemsJson)},${sqlStr(created_at)},${sqlStr(updated_at)},${sqlStr(updated_by)},NULL,1)`,
          );
        }
        const rows = await query(`SELECT * FROM cloud_lists WHERE id='${esc(id)}'`);
        return json(req, { list: serializeList(rows[0]) }, existing[0] ? 200 : 201);
      }

      // Share a list with (or take it back from) the owner's Pocket Family. Owner only.
      const listShareMatch = path.match(/^\/v1\/lists\/([^/]+)\/share$/);
      if (req.method === "POST" && listShareMatch) {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(listShareMatch[1]);
        const row = await accessibleList(id, gate.user.id);
        if (!row || row.deleted_at) return json(req, { message: "List not found." }, 404);
        if (row.user_id !== gate.user.id) {
          return json(req, { message: "Only the person who made this list can change sharing.", code: "not_list_owner" }, 403);
        }
        const body = await req.json().catch(() => ({}));
        if (typeof body?.shared !== "boolean") return json(req, { message: "Say whether to share the list." }, 400);
        let familyId = null;
        if (body.shared) {
          familyId = await familyIdForUser(gate.user.id);
          if (!familyId) return json(req, { message: "Start or join a Pocket Family first." }, 400);
        }
        await query(`UPDATE cloud_lists SET family_id=${sqlNullable(familyId)} WHERE id=${sqlStr(id)}`);
        return json(req, { list: serializeListFor(await accessibleList(id, gate.user.id), gate.user.id) });
      }

      const listMatch = path.match(/^\/v1\/lists\/([^/]+)$/);
      if (listMatch) {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(listMatch[1]);
        if (req.method === "GET") {
          // Owner, or a member of the Pocket Family the list is shared with.
          const row = await accessibleList(id, gate.user.id);
          if (!row) return json(req, { message: "List not found." }, 404);
          return json(req, serializeListFor(row, gate.user.id));
        }
        if (req.method === "PATCH") {
          const body = await req.json().catch(() => ({}));
          const row = await accessibleList(id, gate.user.id);
          const existing = row ? [row] : [];
          if (!existing[0]) return json(req, { message: "List not found." }, 404);
          const title = typeof body.title === "string" ? body.title : existing[0].title;
          const itemsJson = Array.isArray(body.items)
            ? JSON.stringify(body.items)
            : existing[0].items_json;
          const updated_at = typeof body.updated_at === "string" ? body.updated_at : new Date().toISOString();
          const updated_by =
            typeof body.updated_by_device_id === "string" && body.updated_by_device_id
              ? body.updated_by_device_id
              : "pwa";
          await query(
            `UPDATE cloud_lists SET title=${sqlStr(title)}, items_json=${sqlStr(itemsJson)}, updated_at=${sqlStr(updated_at)}, updated_by_device_id=${sqlStr(updated_by)}, version=version+1 WHERE id='${esc(id)}'`,
          );
          return json(req, serializeListFor(await accessibleList(id, gate.user.id), gate.user.id));
        }
        if (req.method === "DELETE") {
          const body = await req.json().catch(() => ({}));
          const row = await accessibleList(id, gate.user.id);
          if (!row) return json(req, { message: "List not found." }, 404);
          if (row.user_id !== gate.user.id) {
            return json(req, { message: "Only the person who made this list can delete it.", code: "not_list_owner" }, 403);
          }
          const updated_at = typeof body.updated_at === "string" ? body.updated_at : new Date().toISOString();
          const updated_by =
            typeof body.updated_by_device_id === "string" && body.updated_by_device_id
              ? body.updated_by_device_id
              : "pwa";
          await query(
            `UPDATE cloud_lists SET deleted_at=${sqlStr(updated_at)}, updated_at=${sqlStr(updated_at)}, updated_by_device_id=${sqlStr(updated_by)}, version=version+1 WHERE id='${esc(id)}'`,
          );
          return new Response(null, { status: 204, headers: cors(req) });
        }
      }

      // ---- Connectors / backups (empty until wired) ----
      if (req.method === "GET" && path === "/v1/connectors") {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const rows = await query(
          `SELECT provider, status, connected_at FROM connector_accounts WHERE user_id='${esc(gate.user.id)}'`,
        );
        const byProv = Object.fromEntries(rows.map((r) => [r.provider, r]));
        const connectors = ["drive", "dropbox", "onedrive"].map((provider) => ({
          provider,
          status: byProv[provider]?.status === "connected" ? "connected" : "disconnected",
          connected_at: byProv[provider]?.connected_at || null,
        }));
        return json(req, { connectors });
      }

      if (req.method === "GET" && path === "/v1/backups") {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const rows = await query(
          `SELECT id, created_at, note_count, list_count, size_bytes FROM backups WHERE user_id='${esc(gate.user.id)}' ORDER BY created_at DESC`,
        );
        return json(req, { backups: rows });
      }

      // Pocket device backups for Companion (newest first, up to 5 kept) + pending "Back up now"s.
      if (req.method === "GET" && path === "/v1/backups/devices") {
        const gate = await requireEntitledUser(req);
        if (gate.error) return gate.error;
        const me = sqlStr(gate.user.id);
        const rows = await query(
          `SELECT id, created_at, device_id, device_name, payload_json, size_bytes FROM device_backups WHERE user_id=${me} ORDER BY created_at DESC, id DESC`,
        );
        const pending = await query(
          `SELECT device_id, backup_requested_at FROM device_links WHERE user_id=${me} AND backup_requested_at IS NOT NULL`,
        );
        return json(req, {
          backups: rows.map((r) => {
            const p = readBackupPayload(r.payload_json);
            return {
              id: r.id,
              created_at: isoTs(r.created_at),
              device_id: r.device_id,
              device_name: r.device_name,
              size_bytes: Number(r.size_bytes || 0),
              settings_count: Object.keys(p.settings).length,
              alarm_count: p.alarms.length,
              reading_count: backupReadingCount(p.reading),
            };
          }),
          pending_requests: pending
            .filter((d) => {
              const t = Date.parse(d.backup_requested_at);
              return Number.isFinite(t) && Date.now() - t <= BACKUP_REQUEST_TTL_MS;
            })
            .map((d) => ({ device_id: d.device_id, requested_at: isoTs(d.backup_requested_at) })),
        });
      }

      // ---- Billing (Stripe via Admin-configured keys or env) ----
      if (req.method === "POST" && path === "/v1/billing/checkout") {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        if (user.disabled_at) return json(req, { message: "This account is disabled." }, 403);
        const cfg = await loadStripeConfig();
        const withTrial = !Number(user.trial_consumed);
        if (cfg.mockMode) {
          const id = `mock_cs_${newId().replace(/-/g, "").slice(0, 24)}`;
          const now = new Date().toISOString();
          await query(
            `INSERT INTO mock_checkouts (id,user_id,with_trial,created_at,completed_at) VALUES ('${esc(id)}','${esc(user.id)}',${withTrial ? 1 : 0},'${now}',NULL)`,
          );
          await completeMockCheckout(id);
          return json(req, { url: `${PWA_ORIGIN}/billing/success?session_id=${id}` });
        }
        if (!cfg.priceMonthlyId) {
          return json(req, { message: "Billing isn't configured yet. Try again later." }, 400);
        }
        let customerId = user.stripe_customer_id;
        if (!customerId) {
          const customer = await stripeFetch(cfg.secretKey, "POST", "/customers", {
            email: user.email,
            "metadata[pocket_user_id]": user.id,
          });
          customerId = customer.id;
          await query(`UPDATE users SET stripe_customer_id='${esc(customerId)}' WHERE id='${esc(user.id)}'`);
        }
        const params = {
          mode: "subscription",
          customer: customerId,
          client_reference_id: user.id,
          success_url: `${PWA_ORIGIN}/billing/success?session_id={CHECKOUT_SESSION_ID}`,
          cancel_url: `${PWA_ORIGIN}/billing/cancel`,
          "line_items[0][price]": cfg.priceMonthlyId,
          "line_items[0][quantity]": "1",
          allow_promotion_codes: "false",
        };
        if (withTrial) params["subscription_data[trial_period_days]"] = "7";
        const session = await stripeFetch(cfg.secretKey, "POST", "/checkout/sessions", params);
        if (!session.url) return json(req, { message: "Couldn't start checkout. Try again." }, 400);
        return json(req, { url: session.url });
      }

      if (req.method === "POST" && path === "/v1/billing/portal") {
        const user = await userFromSession(req);
        if (!user) return json(req, { message: "Sign in to continue." }, 401);
        const cfg = await loadStripeConfig();
        if (cfg.mockMode) {
          if (!user.stripe_customer_id && !Number(user.had_subscription) && !Number(user.trial_consumed)) {
            return json(req, { message: "No billing account yet. Start a subscription first." }, 400);
          }
          return json(req, { url: `${PWA_ORIGIN}/billing?portal=mock` });
        }
        if (!user.stripe_customer_id) {
          return json(req, { message: "No billing account yet. Start a subscription first." }, 400);
        }
        const session = await stripeFetch(cfg.secretKey, "POST", "/billing_portal/sessions", {
          customer: user.stripe_customer_id,
          return_url: `${PWA_ORIGIN}/billing`,
        });
        return json(req, { url: session.url });
      }

      if (req.method === "POST" && path === "/v1/billing/webhook") {
        const cfg = await loadStripeConfig();
        const raw = await req.text();
        if (cfg.mockMode) {
          return json(req, { received: true, mock: true });
        }
        const sig = req.headers.get("stripe-signature") || "";
        if (!verifyStripeWebhookSignature(raw, sig, cfg.webhookSecret)) {
          return json(req, { message: "Invalid webhook signature." }, 400);
        }
        let event;
        try {
          event = JSON.parse(raw);
        } catch {
          return json(req, { message: "Invalid payload." }, 400);
        }
        await handleStripeWebhookEvent(event);
        return json(req, { received: true });
      }

      // ---- Admin ----
      if (req.method === "GET" && path === "/v1/admin/stt") {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const secrets = await loadSttSecrets();
        return json(req, {
          openai_set: Boolean(secrets.openai),
          openai_masked: maskSecret(secrets.openai),
          groq_set: Boolean(secrets.groq),
          groq_masked: maskSecret(secrets.groq),
          neon_ai_gateway: Boolean(
            (process.env.NEON_AI_GATEWAY_TOKEN || "").trim() &&
              (process.env.NEON_AI_GATEWAY_BASE_URL || "").trim(),
          ),
          stt_proxy: Boolean((process.env.STT_PROXY_URL || "").trim()),
          cloudflare_ai: Boolean(
            (process.env.CLOUDFLARE_ACCOUNT_ID || "").trim() &&
              (process.env.CLOUDFLARE_API_TOKEN || "").trim(),
          ),
          hint: "Paste a Groq (free) or OpenAI API key. Neon AI Gateway Gemini is used as fallback when keys are unset.",
        });
      }

      if (req.method === "PUT" && path === "/v1/admin/stt") {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const body = await req.json().catch(() => ({}));
        const updates = [];
        if (typeof body.openai_api_key === "string" && body.openai_api_key.trim()) {
          const k = body.openai_api_key.trim();
          if (!k.startsWith("sk-")) {
            return json(req, { message: "OpenAI key should start with sk-." }, 400);
          }
          await setAppSetting("stt_openai_api_key", k, gate.user.id);
          updates.push("openai_api_key");
        }
        if (typeof body.groq_api_key === "string" && body.groq_api_key.trim()) {
          const k = body.groq_api_key.trim();
          if (!k.startsWith("gsk_")) {
            return json(req, { message: "Groq key should start with gsk_." }, 400);
          }
          await setAppSetting("stt_groq_api_key", k, gate.user.id);
          updates.push("groq_api_key");
        }
        if (body.clear === true) {
          for (const k of STT_SETTING_KEYS) {
            await query(`DELETE FROM app_settings WHERE key='${esc(k)}'`);
          }
          updates.push("cleared");
        }
        if (!updates.length) {
          return json(req, { message: "Nothing to update. Paste openai_api_key, groq_api_key, or clear." }, 400);
        }
        await audit(gate.user.id, "stt.configure", "stt", null, { updates });
        const secrets = await loadSttSecrets();
        return json(req, {
          ok: true,
          updates,
          openai_set: Boolean(secrets.openai),
          openai_masked: maskSecret(secrets.openai),
          groq_set: Boolean(secrets.groq),
          groq_masked: maskSecret(secrets.groq),
        });
      }

      if (req.method === "GET" && path === "/v1/admin/stripe") {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const cfg = await loadStripeConfig();
        const updated = await query(
          `SELECT key, updated_at, updated_by FROM app_settings WHERE key LIKE 'stripe_%' ORDER BY key`,
        );
        return json(req, {
          configured: !cfg.mockMode,
          mock_mode: cfg.mockMode,
          source: cfg.source,
          secret_key_set: Boolean(cfg.secretKey),
          secret_key_masked: maskSecret(cfg.secretKey),
          webhook_secret_set: Boolean(cfg.webhookSecret),
          webhook_secret_masked: maskSecret(cfg.webhookSecret),
          price_monthly_id: cfg.priceMonthlyId || null,
          product_name: cfg.productName,
          webhook_url: `${PUBLIC_BASE_URL || u.origin}/v1/billing/webhook`,
          updated,
        });
      }

      if (req.method === "PUT" && path === "/v1/admin/stripe") {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const body = await req.json().catch(() => ({}));
        const updates = [];
        if (typeof body.secret_key === "string" && body.secret_key.trim()) {
          const sk = body.secret_key.trim();
          if (!sk.startsWith("sk_")) {
            return json(req, { message: "Secret key should start with sk_test_ or sk_live_." }, 400);
          }
          await setAppSetting("stripe_secret_key", sk, gate.user.id);
          updates.push("secret_key");
        }
        if (typeof body.webhook_secret === "string" && body.webhook_secret.trim()) {
          const wh = body.webhook_secret.trim();
          if (!wh.startsWith("whsec_")) {
            return json(req, { message: "Webhook secret should start with whsec_." }, 400);
          }
          await setAppSetting("stripe_webhook_secret", wh, gate.user.id);
          updates.push("webhook_secret");
        }
        if (typeof body.price_monthly_id === "string") {
          const price = body.price_monthly_id.trim();
          if (price && !price.startsWith("price_")) {
            return json(req, { message: "Price id should start with price_." }, 400);
          }
          await setAppSetting("stripe_price_monthly_id", price, gate.user.id);
          updates.push("price_monthly_id");
        }
        if (typeof body.product_name === "string") {
          const name = body.product_name.trim() || "Pocket Cloud";
          if (/connect/i.test(name)) {
            return json(req, { message: 'Product name must not contain "Connect".' }, 400);
          }
          await setAppSetting("stripe_product_name", name, gate.user.id);
          updates.push("product_name");
        }
        if (body.clear === true) {
          for (const k of STRIPE_SETTING_KEYS) {
            await query(`DELETE FROM app_settings WHERE key='${esc(k)}'`);
          }
          updates.push("cleared");
        }
        if (!updates.length) {
          return json(req, { message: "Nothing to update. Paste a key, price id, or clear." }, 400);
        }
        await audit(gate.user.id, "stripe.configure", "stripe", null, { updates });
        const cfg = await loadStripeConfig();
        return json(req, {
          ok: true,
          updates,
          configured: !cfg.mockMode,
          mock_mode: cfg.mockMode,
          source: cfg.source,
          secret_key_masked: maskSecret(cfg.secretKey),
          webhook_secret_masked: maskSecret(cfg.webhookSecret),
          price_monthly_id: cfg.priceMonthlyId || null,
          product_name: cfg.productName,
        });
      }

      if (req.method === "GET" && path === "/v1/admin/overview") {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const users = await query(`SELECT COUNT(*)::text AS c FROM users`);
        const devices = await query(`SELECT COUNT(*)::text AS c FROM device_links`);
        const pending = await query(`SELECT COUNT(*)::text AS c FROM pair_sessions WHERE status='pending' AND expires_at > NOW()`);
        const claimed = await query(`SELECT COUNT(*)::text AS c FROM pair_sessions WHERE status='claimed'`);
        const entitled = await query(`SELECT COUNT(*)::text AS c FROM subscription_mirrors WHERE status IN ('active','trialing')`);
        const badges = await query(`SELECT COUNT(*)::text AS c FROM badges`);
        const awards = await query(`SELECT COUNT(*)::text AS c FROM badge_awards`);
        return json(req, {
          users: Number(users[0]?.c || 0),
          devices: Number(devices[0]?.c || 0),
          pair_pending: Number(pending[0]?.c || 0),
          pair_claimed: Number(claimed[0]?.c || 0),
          subscriptions_active: Number(entitled[0]?.c || 0),
          badges: Number(badges[0]?.c || 0),
          badge_awards: Number(awards[0]?.c || 0),
        });
      }

      if (req.method === "GET" && path === "/v1/admin/users") {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const q = (u.searchParams.get("q") || "").trim().toLowerCase();
        const limit = Math.min(100, Math.max(1, Number(u.searchParams.get("limit") || 50)));
        let sql = `SELECT u.id, u.email, u.role, u.email_verified_at, u.disabled_at, u.created_at, u.last_login_at, u.trial_consumed, u.had_subscription,
          sm.status AS sub_status, sm.trial_ends_at, sm.current_period_end,
          (SELECT COUNT(*)::text FROM device_links d WHERE d.user_id=u.id) AS device_count
          FROM users u
          LEFT JOIN subscription_mirrors sm ON sm.user_id=u.id`;
        if (q) sql += ` WHERE u.email LIKE '%${esc(q)}%' OR u.id='${esc(q)}'`;
        sql += ` ORDER BY u.created_at DESC LIMIT ${limit}`;
        const rows = await query(sql);
        return json(req, {
          users: rows.map((r) => ({
            id: r.id,
            email: r.email,
            role: r.role || "user",
            email_verified: Boolean(r.email_verified_at),
            disabled: Boolean(r.disabled_at),
            created_at: r.created_at,
            last_login_at: r.last_login_at || null,
            trial_consumed: Boolean(Number(r.trial_consumed)),
            had_subscription: Boolean(Number(r.had_subscription)),
            entitlement_status: r.sub_status || "free",
            trial_ends_at: r.trial_ends_at || null,
            current_period_end: r.current_period_end || null,
            device_count: Number(r.device_count || 0),
          })),
        });
      }

      const userMatch = path.match(/^\/v1\/admin\/users\/([^/]+)$/);
      if (req.method === "GET" && userMatch) {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(userMatch[1]);
        const rows = await query(`SELECT * FROM users WHERE id='${esc(id)}'`);
        const user = rows[0];
        if (!user) return json(req, { message: "User not found." }, 404);
        const devices = await query(`SELECT id, device_id, device_name, linked_at, last_seen_at FROM device_links WHERE user_id='${esc(id)}' ORDER BY linked_at DESC`);
        const subs = await query(`SELECT * FROM subscription_mirrors WHERE user_id='${esc(id)}'`);
        const awards = await query(`SELECT a.id, a.badge_id, a.note, a.awarded_at, b.key, b.name, b.icon_key FROM badge_awards a JOIN badges b ON b.id=a.badge_id WHERE a.user_id='${esc(id)}' ORDER BY a.awarded_at DESC`);
        const sub = subs[0];
        return json(req, {
          user: {
            id: user.id,
            email: user.email,
            role: user.role || "user",
            email_verified: Boolean(user.email_verified_at),
            disabled: Boolean(user.disabled_at),
            created_at: user.created_at,
            last_login_at: user.last_login_at || null,
            trial_consumed: Boolean(Number(user.trial_consumed)),
            had_subscription: Boolean(Number(user.had_subscription)),
            stripe_customer_id: user.stripe_customer_id || null,
          },
          entitlement: {
            status: sub?.status || "free",
            entitled: sub?.status === "active" || sub?.status === "trialing",
            trial_ends_at: sub?.trial_ends_at || null,
            current_period_end: sub?.current_period_end || null,
          },
          devices,
          badges: awards,
        });
      }

      if (req.method === "PATCH" && userMatch) {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(userMatch[1]);
        const rows = await query(`SELECT * FROM users WHERE id='${esc(id)}'`);
        const user = rows[0];
        if (!user) return json(req, { message: "User not found." }, 404);
        const body = await req.json().catch(() => ({}));
        const now = new Date().toISOString();
        if (body.verify_email === true) {
          await query(`UPDATE users SET email_verified_at=COALESCE(email_verified_at,'${now}') WHERE id='${esc(id)}'`);
          await audit(gate.user.id, "verify_email", "user", id, null);
        }
        if (body.disabled === true) {
          await query(`UPDATE users SET disabled_at='${now}' WHERE id='${esc(id)}'`);
          await query(`DELETE FROM sessions WHERE user_id='${esc(id)}'`);
          await audit(gate.user.id, "disable_user", "user", id, null);
        } else if (body.disabled === false) {
          await query(`UPDATE users SET disabled_at=NULL WHERE id='${esc(id)}'`);
          await audit(gate.user.id, "enable_user", "user", id, null);
        }
        if (body.role === "admin" || body.role === "user") {
          if (id === gate.user.id && body.role !== "admin") {
            return json(req, { message: "You can't remove your own admin role." }, 400);
          }
          await query(`UPDATE users SET role='${esc(body.role)}' WHERE id='${esc(id)}'`);
          await audit(gate.user.id, "set_role", "user", id, { role: body.role });
        }
        if (typeof body.entitlement_status === "string") {
          const st = body.entitlement_status;
          if (!["free", "trialing", "active", "past_due", "canceled", "lapsed"].includes(st)) {
            return json(req, { message: "Invalid entitlement status." }, 400);
          }
          if (st === "free" || st === "lapsed") {
            await query(`DELETE FROM subscription_mirrors WHERE user_id='${esc(id)}'`);
            if (st === "lapsed") await query(`UPDATE users SET had_subscription=1 WHERE id='${esc(id)}'`);
          } else {
            const existing = await query(`SELECT user_id FROM subscription_mirrors WHERE user_id='${esc(id)}'`);
            const trialEnd = st === "trialing" ? new Date(Date.now() + 7 * 86400e3).toISOString() : null;
            // active = permanent/comped grant (no Stripe period end). Trial keeps 7-day end.
            if (existing[0]) {
              await query(
                `UPDATE subscription_mirrors SET status='${esc(st)}', trial_ends_at=${sqlNullable(trialEnd)}, current_period_end=${st === "active" ? "NULL" : "current_period_end"}, updated_at='${now}' WHERE user_id='${esc(id)}'`,
              );
            } else {
              await query(`INSERT INTO subscription_mirrors (user_id,stripe_subscription_id,status,trial_ends_at,current_period_end,cancel_at_period_end,updated_at) VALUES ('${esc(id)}',NULL,'${esc(st)}',${sqlNullable(trialEnd)},NULL,0,'${now}')`);
            }
            if (st === "active" || st === "trialing") {
              await query(`UPDATE users SET had_subscription=1${st === "trialing" ? ", trial_consumed=1" : ""} WHERE id='${esc(id)}'`);
            }
          }
          await audit(gate.user.id, "set_entitlement", "user", id, { status: st });
        }
        const fresh = await query(`SELECT * FROM users WHERE id='${esc(id)}'`);
        const f = fresh[0];
        const subs = await query(`SELECT status, trial_ends_at, current_period_end FROM subscription_mirrors WHERE user_id='${esc(id)}'`);
        const sub = subs[0];
        return json(req, {
          user: {
            id: f.id,
            email: f.email,
            role: f.role || "user",
            email_verified: Boolean(f.email_verified_at),
            disabled: Boolean(f.disabled_at),
            created_at: f.created_at,
            last_login_at: f.last_login_at || null,
            trial_consumed: Boolean(Number(f.trial_consumed)),
          },
          entitlement: {
            status: sub?.status || "free",
            entitled: sub?.status === "active" || sub?.status === "trialing",
            trial_ends_at: sub?.trial_ends_at || null,
            current_period_end: sub?.current_period_end || null,
          },
        });
      }

const ADMIN_DEVICE_COLUMNS = `d.id, d.device_id, d.device_name, d.linked_at, d.last_seen_at, d.user_id, u.email AS user_email,
          d.volume_percent, d.brightness_percent, d.lock_message, d.parental_json, d.sd_present,
          d.pending_wifi_ssid, d.pending_wifi_at`;

function shapeAdminDevice(row) {
  if (!row) return row;
  let parental = {};
  try {
    parental = JSON.parse(row.parental_json || "{}") || {};
  } catch {
    parental = {};
  }
  return {
    id: row.id,
    device_id: row.device_id,
    device_name: row.device_name,
    linked_at: row.linked_at,
    last_seen_at: row.last_seen_at,
    user_id: row.user_id,
    user_email: row.user_email,
    volume_percent: Number(row.volume_percent ?? 80),
    brightness_percent: Number(row.brightness_percent ?? 50),
    lock_message: row.lock_message || "",
    parental,
    sd_present: Boolean(row.sd_present),
    pending_wifi_ssid: row.pending_wifi_ssid || null,
    pending_wifi_at: row.pending_wifi_at || null,
  };
}

      if (req.method === "GET" && path === "/v1/admin/devices") {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const userId = (u.searchParams.get("user_id") || "").trim();
        let sql = `SELECT ${ADMIN_DEVICE_COLUMNS} FROM device_links d JOIN users u ON u.id=d.user_id`;
        if (userId) sql += ` WHERE d.user_id='${esc(userId)}'`;
        sql += ` ORDER BY d.linked_at DESC LIMIT 200`;
        const devices = await query(sql);
        return json(req, { devices: devices.map(shapeAdminDevice) });
      }

      const deviceMatch = path.match(/^\/v1\/admin\/devices\/([^/]+)$/);
      if (req.method === "PATCH" && deviceMatch) {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(deviceMatch[1]);
        const body = await req.json().catch(() => ({}));
        const rows = await query(`SELECT * FROM device_links WHERE id='${esc(id)}'`);
        const row = rows[0];
        if (!row) return json(req, { message: "Device not found." }, 404);

        // Map keyed by column so a client can never send two updates for the same
        // column in one PATCH (Postgres rejects `SET x=1, x=2` in one statement).
        const sets = new Map();
        const changes = {};

        if (body.reset === true) {
          sets.set("lock_message", `''`);
          sets.set("volume_percent", "80");
          sets.set("brightness_percent", "50");
          sets.set("parental_json", "'{}'");
          sets.set("pending_wifi_ssid", "NULL");
          sets.set("pending_wifi_password", "NULL");
          sets.set("pending_wifi_at", "NULL");
          changes.reset = true;
        }
        if (typeof body.device_name === "string") {
          const name = body.device_name.trim();
          if (!name || name.length > 64) return json(req, { message: "Enter a device label (1–64 characters)." }, 400);
          sets.set("device_name", sqlStr(name));
          changes.device_name = name;
        }
        if (Number.isFinite(body.volume_percent)) {
          const v = Math.max(0, Math.min(100, Math.round(body.volume_percent)));
          sets.set("volume_percent", String(v));
          changes.volume_percent = v;
        }
        if (Number.isFinite(body.brightness_percent)) {
          const v = Math.max(0, Math.min(100, Math.round(body.brightness_percent)));
          sets.set("brightness_percent", String(v));
          changes.brightness_percent = v;
        }
        if (typeof body.lock_message === "string") {
          const msg = body.lock_message.trim().slice(0, 40);
          sets.set("lock_message", sqlStr(msg));
          changes.lock_message = msg;
        }
        if (body.unlock_parental === true) {
          sets.set("parental_json", "'{}'");
          changes.parental = "cleared";
        } else if (body.parental && typeof body.parental === "object") {
          const pin_gated_apps = Array.isArray(body.parental.pin_gated_apps)
            ? body.parental.pin_gated_apps.map((a) => String(a).trim().toLowerCase()).filter(Boolean).slice(0, 16)
            : [];
          const parental = {
            pin_gated_apps,
            hide_pass_share: body.parental.hide_pass_share === true,
            block_connectors: body.parental.block_connectors === true,
          };
          sets.set("parental_json", sqlStr(JSON.stringify(parental)));
          changes.parental = parental;
        }
        if (typeof body.sd_present === "boolean") {
          sets.set("sd_present", body.sd_present ? "true" : "false");
          changes.sd_present = body.sd_present;
        }
        if (typeof body.ssid === "string" && body.ssid.trim()) {
          const ssid = body.ssid.trim().slice(0, 32);
          const password = typeof body.password === "string" ? body.password.slice(0, 64) : "";
          sets.set("pending_wifi_ssid", sqlStr(ssid));
          sets.set("pending_wifi_password", sqlStr(password));
          sets.set("pending_wifi_at", `'${new Date().toISOString()}'`);
          changes.pending_wifi_ssid = ssid;
        }
        if (typeof body.user_id === "string" && body.user_id.trim() && body.user_id.trim() !== row.user_id) {
          const newOwner = await query(`SELECT id FROM users WHERE id='${esc(body.user_id.trim())}'`);
          if (!newOwner[0]) return json(req, { message: "Target user not found." }, 404);
          sets.set("user_id", `'${esc(body.user_id.trim())}'`);
          changes.reassigned_to_user_id = body.user_id.trim();
        }

        if (!sets.size) return json(req, { message: "Nothing to update." }, 400);
        const setClause = [...sets.entries()].map(([col, val]) => `${col}=${val}`).join(", ");
        await query(`UPDATE device_links SET ${setClause} WHERE id='${esc(id)}'`);
        await audit(gate.user.id, "edit_device", "device", id, changes);
        const fresh = await query(`SELECT ${ADMIN_DEVICE_COLUMNS} FROM device_links d JOIN users u ON u.id=d.user_id WHERE d.id='${esc(id)}'`);
        return json(req, { device: shapeAdminDevice(fresh[0]) });
      }

      if (req.method === "DELETE" && deviceMatch) {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(deviceMatch[1]);
        const rows = await query(`SELECT id, device_id, user_id FROM device_links WHERE id='${esc(id)}'`);
        if (!rows[0]) return json(req, { message: "Device not found." }, 404);
        await query(`DELETE FROM badge_awards WHERE device_link_id='${esc(id)}'`);
        await query(`DELETE FROM device_links WHERE id='${esc(id)}'`);
        await audit(gate.user.id, "unlink_device", "device", id, { device_id: rows[0].device_id, user_id: rows[0].user_id });
        return json(req, { ok: true });
      }

      // Admin bypass: directly attach a device_id to a user's account without the
      // phone-scan pairing flow (device only needs the shared device key + device_id
      // to authenticate, so this takes effect immediately for the firmware).
      if (req.method === "POST" && path === "/v1/admin/devices/force-link") {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const body = await req.json().catch(() => ({}));
        const deviceId = typeof body.device_id === "string" ? body.device_id.trim() : "";
        const userId = typeof body.user_id === "string" ? body.user_id.trim() : "";
        const deviceName = typeof body.device_name === "string" && body.device_name.trim() ? body.device_name.trim().slice(0, 20) : "Pocket";
        if (!deviceId || !userId) return json(req, { message: "device_id and user_id are required." }, 400);
        const userRows = await query(`SELECT id FROM users WHERE id='${esc(userId)}'`);
        if (!userRows[0]) return json(req, { message: "Target user not found." }, 404);
        const now = new Date().toISOString();
        const existing = await query(`SELECT id FROM device_links WHERE device_id='${esc(deviceId)}'`);
        let linkId;
        if (existing[0]) {
          linkId = existing[0].id;
          await query(`UPDATE device_links SET user_id='${esc(userId)}', device_name=${sqlStr(deviceName)}, last_seen_at='${now}' WHERE id='${esc(linkId)}'`);
        } else {
          linkId = newId();
          await query(`INSERT INTO device_links (id,user_id,device_id,device_name,linked_at,last_seen_at,device_token_hash) VALUES ('${esc(linkId)}','${esc(userId)}','${esc(deviceId)}',${sqlStr(deviceName)},'${now}','${now}','${esc(sha256Hex(randomToken(32)))}')`);
        }
        await audit(gate.user.id, "force_link_device", "device", linkId, { device_id: deviceId, user_id: userId });
        const fresh = await query(`SELECT ${ADMIN_DEVICE_COLUMNS} FROM device_links d JOIN users u ON u.id=d.user_id WHERE d.id='${esc(linkId)}'`);
        return json(req, { device: shapeAdminDevice(fresh[0]) }, existing[0] ? 200 : 201);
      }

      if (req.method === "GET" && path === "/v1/admin/pair-sessions") {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const status = (u.searchParams.get("status") || "").trim();
        let sql = `SELECT id, device_id, code_public_hint, status, created_at, expires_at, claimed_at, claimed_by_user_id FROM pair_sessions`;
        if (status === "pending") sql += ` WHERE status='pending'`;
        else if (status === "claimed") sql += ` WHERE status='claimed'`;
        else if (status === "expired") sql += ` WHERE status='expired' OR (status='pending' AND expires_at < NOW())`;
        sql += ` ORDER BY created_at DESC LIMIT 100`;
        const sessions = await query(sql);
        return json(req, { sessions });
      }

      // Admin bypass: finish a stuck pairing code for a chosen user without the
      // phone completing the normal claim (e.g. phone lost network mid-pair).
      const pairForceClaimMatch = path.match(/^\/v1\/admin\/pair-sessions\/([^/]+)\/claim$/);
      if (req.method === "POST" && pairForceClaimMatch) {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(pairForceClaimMatch[1]);
        const body = await req.json().catch(() => ({}));
        const userId = typeof body.user_id === "string" ? body.user_id.trim() : "";
        if (!userId) return json(req, { message: "user_id is required." }, 400);
        const userRows = await query(`SELECT id FROM users WHERE id='${esc(userId)}'`);
        if (!userRows[0]) return json(req, { message: "Target user not found." }, 404);
        const rows = await query(`SELECT * FROM pair_sessions WHERE id='${esc(id)}'`);
        const row = rows[0];
        if (!row) return json(req, { message: "Pairing session not found." }, 404);
        const now = new Date().toISOString();
        const existing = await query(`SELECT id FROM device_links WHERE device_id='${esc(row.device_id)}'`);
        if (existing[0]) {
          await query(`UPDATE device_links SET user_id='${esc(userId)}', last_seen_at='${now}' WHERE id='${esc(existing[0].id)}'`);
        } else {
          await query(`INSERT INTO device_links (id,user_id,device_id,device_name,linked_at,last_seen_at,device_token_hash) VALUES ('${esc(newId())}','${esc(userId)}','${esc(row.device_id)}','Pocket','${now}','${now}','${esc(sha256Hex(randomToken(32)))}')`);
        }
        await query(`UPDATE pair_sessions SET status='claimed', claimed_at='${now}', claimed_by_user_id='${esc(userId)}' WHERE id='${esc(row.id)}'`);
        await audit(gate.user.id, "force_claim_pairing", "pair_session", id, { device_id: row.device_id, user_id: userId });
        return json(req, { ok: true });
      }

      // Admin bypass: cancel/expire a pairing code (e.g. a stale or wrong one on
      // the shared "period without updates" — frees the code without waiting on TTL).
      const pairCancelMatch = path.match(/^\/v1\/admin\/pair-sessions\/([^/]+)$/);
      if (req.method === "DELETE" && pairCancelMatch) {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(pairCancelMatch[1]);
        const rows = await query(`SELECT id, device_id FROM pair_sessions WHERE id='${esc(id)}'`);
        if (!rows[0]) return json(req, { message: "Pairing session not found." }, 404);
        await query(`UPDATE pair_sessions SET status='expired' WHERE id='${esc(id)}'`);
        await audit(gate.user.id, "cancel_pairing_session", "pair_session", id, { device_id: rows[0].device_id });
        return json(req, { ok: true });
      }

      if (req.method === "GET" && path === "/v1/admin/badges") {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const badges = await query(`SELECT b.*, (SELECT COUNT(*)::text FROM badge_awards a WHERE a.badge_id=b.id) AS award_count FROM badges b ORDER BY b.name ASC`);
        const awards = await query(`SELECT a.id, a.badge_id, a.user_id, a.device_link_id, a.note, a.awarded_at, a.awarded_by, b.name AS badge_name, b.key AS badge_key, b.icon_key, u.email AS user_email
          FROM badge_awards a
          JOIN badges b ON b.id=a.badge_id
          LEFT JOIN users u ON u.id=a.user_id
          ORDER BY a.awarded_at DESC LIMIT 100`);
        return json(req, {
          badges: badges.map((b) => ({ ...b, award_count: Number(b.award_count || 0) })),
          awards,
        });
      }

      if (req.method === "POST" && path === "/v1/admin/badges") {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const body = await req.json().catch(() => ({}));
        const name = typeof body.name === "string" ? body.name.trim() : "";
        const key = typeof body.key === "string" ? body.key.trim().toLowerCase().replace(/[^a-z0-9_]+/g, "_") : "";
        const icon_key = typeof body.icon_key === "string" && body.icon_key.trim() ? body.icon_key.trim() : "star";
        const description = typeof body.description === "string" ? body.description.trim() : "";
        if (!name || !key) return json(req, { message: "Name and key are required." }, 400);
        const exists = await query(`SELECT id FROM badges WHERE key='${esc(key)}'`);
        if (exists[0]) return json(req, { message: "A badge with that key already exists." }, 400);
        const id = newId(), now = new Date().toISOString();
        await query(`INSERT INTO badges (id,key,name,icon_key,description,created_at,updated_at) VALUES ('${esc(id)}','${esc(key)}','${esc(name)}','${esc(icon_key)}','${esc(description)}','${now}','${now}')`);
        await audit(gate.user.id, "create_badge", "badge", id, { key, name });
        return json(req, { badge: { id, key, name, icon_key, description, created_at: now, updated_at: now } }, 201);
      }

      const badgeMatch = path.match(/^\/v1\/admin\/badges\/([^/]+)$/);
      if (req.method === "PATCH" && badgeMatch) {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(badgeMatch[1]);
        const rows = await query(`SELECT * FROM badges WHERE id='${esc(id)}'`);
        if (!rows[0]) return json(req, { message: "Badge not found." }, 404);
        const body = await req.json().catch(() => ({}));
        const name = typeof body.name === "string" ? body.name.trim() : rows[0].name;
        const icon_key = typeof body.icon_key === "string" ? body.icon_key.trim() : rows[0].icon_key;
        const description = typeof body.description === "string" ? body.description.trim() : rows[0].description;
        const now = new Date().toISOString();
        await query(`UPDATE badges SET name='${esc(name)}', icon_key='${esc(icon_key)}', description='${esc(description)}', updated_at='${now}' WHERE id='${esc(id)}'`);
        await audit(gate.user.id, "edit_badge", "badge", id, { name });
        const fresh = await query(`SELECT * FROM badges WHERE id='${esc(id)}'`);
        return json(req, { badge: fresh[0] });
      }

      const awardMatch = path.match(/^\/v1\/admin\/badges\/([^/]+)\/award$/);
      if (req.method === "POST" && awardMatch) {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const badgeId = decodeURIComponent(awardMatch[1]);
        const badges = await query(`SELECT id FROM badges WHERE id='${esc(badgeId)}'`);
        if (!badges[0]) return json(req, { message: "Badge not found." }, 404);
        const body = await req.json().catch(() => ({}));
        const userId = typeof body.user_id === "string" ? body.user_id.trim() : "";
        const deviceLinkId = typeof body.device_link_id === "string" ? body.device_link_id.trim() : "";
        const note = typeof body.note === "string" ? body.note.trim() : "";
        if (!userId && !deviceLinkId) return json(req, { message: "Choose a user or device to award." }, 400);
        if (userId) {
          const urows = await query(`SELECT id FROM users WHERE id='${esc(userId)}'`);
          if (!urows[0]) return json(req, { message: "User not found." }, 404);
        }
        if (deviceLinkId) {
          const drows = await query(`SELECT id FROM device_links WHERE id='${esc(deviceLinkId)}'`);
          if (!drows[0]) return json(req, { message: "Device not found." }, 404);
        }
        const id = newId(), now = new Date().toISOString();
        await query(`INSERT INTO badge_awards (id,badge_id,user_id,device_link_id,awarded_by,note,awarded_at) VALUES ('${esc(id)}','${esc(badgeId)}',${sqlNullable(userId)},${sqlNullable(deviceLinkId)},'${esc(gate.user.id)}',${sqlNullable(note)},'${now}')`);
        await audit(gate.user.id, "award_badge", "badge_award", id, { badge_id: badgeId, user_id: userId || null, device_link_id: deviceLinkId || null });
        return json(req, { award: { id, badge_id: badgeId, user_id: userId || null, device_link_id: deviceLinkId || null, note: note || null, awarded_at: now } }, 201);
      }

      const awardIdMatch = path.match(/^\/v1\/admin\/badge-awards\/([^/]+)$/);
      if (req.method === "DELETE" && awardIdMatch) {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const id = decodeURIComponent(awardIdMatch[1]);
        const rows = await query(`SELECT id FROM badge_awards WHERE id='${esc(id)}'`);
        if (!rows[0]) return json(req, { message: "Award not found." }, 404);
        await query(`DELETE FROM badge_awards WHERE id='${esc(id)}'`);
        await audit(gate.user.id, "revoke_badge", "badge_award", id, null);
        return json(req, { ok: true });
      }

      if (req.method === "GET" && path === "/v1/admin/activity") {
        const gate = await requireAdmin(req);
        if (gate.error) return gate.error;
        const logins = await query(`SELECT id, email, last_login_at FROM users WHERE last_login_at IS NOT NULL ORDER BY last_login_at DESC LIMIT 25`);
        const claims = await query(`SELECT p.id, p.device_id, p.claimed_at, p.claimed_by_user_id, u.email AS user_email
          FROM pair_sessions p LEFT JOIN users u ON u.id=p.claimed_by_user_id
          WHERE p.status='claimed' ORDER BY p.claimed_at DESC NULLS LAST LIMIT 25`);
        const audits = await query(`SELECT a.*, u.email AS actor_email FROM admin_audit_log a LEFT JOIN users u ON u.id=a.actor_user_id ORDER BY a.created_at DESC LIMIT 40`);
        return json(req, { logins, pair_claims: claims, audits });
      }


      return json(req, { error: "not_found", path }, 404);
    } catch (e) {
      try { if (sock) sock.destroy(); } catch {}
      sock = null;
      return json(req, { error: "internal", message: String(e.message || e) }, 500);
    }
  },
};