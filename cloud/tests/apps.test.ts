import { describe, expect, it, beforeEach, afterEach, vi } from "vitest";
import { createHmac } from "node:crypto";
import { openMemoryDatabase, closeDatabase, run } from "../src/db/index.js";
import { createApp } from "../src/app.js";
import { getOrCreateUser, createSession } from "../src/lib/auth.js";
import { sha256Hex } from "../src/lib/crypto.js";
import { appReturnUrl, ingestRegistry, parseAppFeed, parseAppManifest } from "../src/lib/apps.js";

type App = ReturnType<typeof createApp>;
const icon = Buffer.concat([Buffer.from("P4\n32 32\n"), Buffer.alloc(128, 0x81)]).toString("base64");
const manifest = (over: Record<string, unknown> = {}) => ({
  schema: "pocket-app/1",
  id: "dev.example.weatherline",
  version: "1.0.0",
  name: "Weatherline",
  summary: "One line of weather.",
  developer: { id: "dev_1", name: "Example" },
  icon_pbm_b64: icon,
  data_url: "https://feeds.example.dev/pocket",
  refresh_minutes: 30,
  auth: { type: "none" },
  surfaces: ["app", "today"],
  approved_at: "2026-11-25T00:00:00Z",
  ...over,
});
const feed = {
  schema: "pocket-feed/1",
  title: "Weatherline",
  today: "Sunny, 12°",
  screens: [
    {
      id: "main",
      title: "Today",
      blocks: [
        { type: "header", text: "Now" },
        { type: "kv", key: "Temp", value: "12°" },
        { type: "row", text: "Week ahead", detail: "7 days", go: "week" },
        { type: "sparkle", text: "from a newer feed" },
        { type: "divider" },
      ],
    },
    { id: "week", title: "Week", blocks: [{ type: "empty", text: "Nothing yet" }] },
  ],
};

describe("app manifest + feed validation", () => {
  it("accepts a good manifest and rejects bad ones", () => {
    expect(parseAppManifest(manifest())?.surfaces).toEqual(["app", "today"]);
    expect(parseAppManifest(manifest({ data_url: "http://insecure.example" }))).toBeNull();
    expect(parseAppManifest(manifest({ name: "A name that is far too long" }))).toBeNull();
    expect(parseAppManifest(manifest({ icon_pbm_b64: "abc" }))).toBeNull();
    expect(parseAppManifest(manifest({ auth: { type: "oauth2", provider: "unknown" } }))).toBeNull();
    expect(parseAppManifest(manifest({ auth: { type: "oauth2", provider: "guide", scopes: ["openid"] } }))?.auth).toEqual({
      type: "oauth2",
      provider: "guide",
      scopes: ["openid"],
    });
  });
  it("validates feeds, skips unknown blocks, strips emoji", () => {
    const f = parseAppFeed({ ...feed, today: "Sunny 😀" })!;
    expect(f.today).toBe("Sunny");
    expect(f.screens[0].blocks.map((b) => b.type)).toEqual(["header", "kv", "row", "divider"]);
    expect(parseAppFeed({ ...feed, screens: [{ id: "a", title: "A", blocks: [{ type: "row", text: "x", go: "nope" }] }] })).toBeNull();
    expect(parseAppFeed({ ...feed, title: "x".repeat(41) })).toBeNull();
  });
  it("only returns to Northwork hosts", () => {
    expect(appReturnUrl("https://northwork.dev/store/x")).toBe("https://northwork.dev/store/x");
    expect(appReturnUrl("https://evil.example/")).toBe("https://northwork.app/pocket/app/apps");
  });
});

describe("apps API", () => {
  let app: App;
  let session: Record<string, string>;
  const secret = "registry-secret";

  beforeEach(async () => {
    closeDatabase();
    await openMemoryDatabase();
    app = createApp();
    const user = getOrCreateUser("apps@example.com");
    session = { "x-pocket-session": createSession(user.id).token };
    run(
      `INSERT INTO device_links (id, user_id, device_id, device_name, linked_at, last_seen_at, device_token_hash)
       VALUES ('l1', ?, 'dev-a', 'Pocket', ?, NULL, ?)`,
      [user.id, new Date().toISOString(), sha256Hex("tok-a")],
    );
    const raw = JSON.stringify({ generated_at: "2026-11-25T00:00:00Z", apps: [manifest(), manifest({ id: "dev.example.gone" })], revoked: ["dev.example.gone"] });
    expect(ingestRegistry(raw, "bad", secret)).toBe(false);
    expect(ingestRegistry(raw, createHmac("sha256", secret).update(raw).digest("hex"), secret)).toBe(true);
  });
  afterEach(() => vi.unstubAllGlobals());

  async function call(method: string, path: string, headers: Record<string, string>, body?: unknown) {
    const res = await app.request(`http://localhost${path}`, {
      method,
      headers: { "content-type": "application/json", ...headers },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    return { status: res.status, body: (await res.json()) as any };
  }

  it("lists approved apps, installs one, and serves its validated feed to the Pocket", async () => {
    const list = await call("GET", "/v1/apps", session);
    expect(list.body.available.map((a: any) => a.id)).toEqual(["dev.example.weatherline"]); // revoked one is gone
    const inst = await call("POST", "/v1/apps/install", session, { id: "dev.example.weatherline", devices: "all" });
    expect(inst.body).toEqual({ ok: true, needs_auth: false });

    const fetchMock = vi.fn(async (_url: string, init: any) => {
      expect(init.headers["x-pocket-user"]).toMatch(/^[0-9a-f]{32}$/);
      return new Response(JSON.stringify(feed), { status: 200, headers: { "content-type": "application/json" } });
    });
    vi.stubGlobal("fetch", fetchMock);
    const dev = await call("GET", "/v1/device/apps", { "x-device-token": "tok-a" });
    expect(dev.status).toBe(200);
    expect(dev.body.apps).toHaveLength(1);
    expect(dev.body.apps[0].status).toBe("ok");
    expect(dev.body.apps[0].feed.today).toBe("Sunny, 12°");
    // Cached until refresh_minutes pass.
    await call("GET", "/v1/device/apps", { "x-device-token": "tok-a" });
    expect(fetchMock).toHaveBeenCalledTimes(1);

    expect((await call("POST", "/v1/apps/uninstall", session, { id: "dev.example.weatherline" })).body.ok).toBe(true);
    expect((await call("GET", "/v1/device/apps", { "x-device-token": "tok-a" })).body.apps).toEqual([]);
  });

  it("keeps the last good feed when the developer sends a broken one", async () => {
    await call("POST", "/v1/apps/install", session, { id: "dev.example.weatherline" });
    vi.stubGlobal("fetch", vi.fn(async () => new Response(JSON.stringify(feed), { status: 200 })));
    await call("GET", "/v1/device/apps", { "x-device-token": "tok-a" });
    run("UPDATE app_feeds SET fetched_at = ?", ["2000-01-01T00:00:00.000Z"]);
    vi.stubGlobal("fetch", vi.fn(async () => new Response("{not json", { status: 200 })));
    const again = await call("GET", "/v1/device/apps", { "x-device-token": "tok-a" });
    expect(again.body.apps[0].feed.title).toBe("Weatherline");
  });

  it("refuses unknown apps and other people's Pockets", async () => {
    expect((await call("POST", "/v1/apps/install", session, { id: "nope" })).status).toBe(404);
    expect((await call("POST", "/v1/apps/install", session, { id: "dev.example.weatherline", devices: ["dev-x"] })).status).toBe(403);
    expect((await call("GET", "/v1/device/apps", {})).status).toBe(401);
  });
});
