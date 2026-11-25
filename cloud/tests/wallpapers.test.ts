import { describe, expect, it, beforeEach } from "vitest";
import { openMemoryDatabase, closeDatabase, run } from "../src/db/index.js";
import { createApp } from "../src/app.js";
import { getOrCreateUser, createSession } from "../src/lib/auth.js";
import { sha256Hex } from "../src/lib/crypto.js";
import { parseWallLayout, parseWallPbm, WALL_BYTES } from "../src/lib/wallpapers.js";

type App = ReturnType<typeof createApp>;

const pbm = (fill = 0) =>
  Buffer.concat([Buffer.from("P4\n480 800\n"), Buffer.alloc(WALL_BYTES, fill)]).toString("base64");

function linkDevice(userId: string, deviceId: string, token: string) {
  run(
    `INSERT INTO device_links (id, user_id, device_id, device_name, linked_at, last_seen_at, device_token_hash)
     VALUES (?, ?, ?, 'Pocket', ?, NULL, ?)`,
    [`link-${deviceId}`, userId, deviceId, new Date().toISOString(), sha256Hex(token)],
  );
}

async function call(app: App, method: string, path: string, headers: Record<string, string>, body?: unknown) {
  const res = await app.request(`http://localhost${path}`, {
    method,
    headers: { "content-type": "application/json", ...headers },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const buf = Buffer.from(await res.arrayBuffer());
  const ct = res.headers.get("content-type") || "";
  return { status: res.status, body: ct.includes("json") ? JSON.parse(buf.toString() || "null") : buf } as any;
}

describe("wallpaper validation", () => {
  it("accepts exactly 480×800 P4 and normalizes the header", () => {
    expect(parseWallPbm(pbm())?.length).toBe(11 + WALL_BYTES);
    const spaced = Buffer.concat([Buffer.from("P4 480\t800\n"), Buffer.alloc(WALL_BYTES)]).toString("base64");
    expect(parseWallPbm(spaced)?.subarray(0, 11).toString()).toBe("P4\n480 800\n");
    expect(parseWallPbm(Buffer.from("P4\n800 480\n").toString("base64"))).toBeNull();
    expect(parseWallPbm(Buffer.concat([Buffer.from("P4\n480 800\n"), Buffer.alloc(10)]).toString("base64"))).toBeNull();
  });
  it("checks the clock box stays on screen", () => {
    expect(parseWallLayout({ clock: { x: 40, y: 520, w: 400, h: 160, style: "large", ink: "white" } })?.clock?.ink).toBe("white");
    expect(parseWallLayout({ clock: { x: 200, y: 0, w: 400, h: 160 } })).toBeNull();
    expect(parseWallLayout({ clock: { x: 0, y: 0, w: 100, h: 160 } })).toBeNull();
    expect(parseWallLayout(null)?.clock).toBeNull();
  });
});

describe("wallpapers API", () => {
  let app: App;
  let session: Record<string, string>;
  let userId: string;
  const dev = { "x-device-token": "tok-a" };

  beforeEach(async () => {
    closeDatabase();
    await openMemoryDatabase();
    app = createApp();
    const user = getOrCreateUser("walls@example.com");
    userId = user.id;
    session = { "x-pocket-session": createSession(user.id).token };
    linkDevice(userId, "dev-a", "tok-a");
    linkDevice(userId, "dev-b", "tok-b");
  });

  it("stores a wallpaper and delivers it only to the chosen Pocket", async () => {
    const created = await call(app, "POST", "/v1/wallpapers", session, {
      name: "Fern at dawn",
      image_pbm_b64: pbm(0xaa),
      layout: { clock: { x: 40, y: 520, w: 400, h: 160 } },
      devices: ["dev-a"],
    });
    expect(created.status).toBe(201);
    const wp = created.body.wallpaper;
    expect(wp.id).toMatch(/^wp_/);
    expect(wp.devices).toEqual(["dev-a"]);
    expect(wp.layout.clock.style).toBe("large");

    const listA = await call(app, "GET", "/v1/device/wallpapers", dev);
    expect(listA.body.wallpapers.map((w: any) => w.id)).toEqual([wp.id]);
    const listB = await call(app, "GET", "/v1/device/wallpapers", { "x-device-token": "tok-b" });
    expect(listB.body.wallpapers).toEqual([]);

    const img = await call(app, "GET", `/v1/device/wallpaper/${wp.id}`, dev);
    expect(img.status).toBe(200);
    expect(img.body.length).toBe(11 + WALL_BYTES);
    expect((await call(app, "GET", `/v1/device/wallpaper/${wp.id}`, { "x-device-token": "tok-b" })).status).toBe(404);
    expect((await call(app, "GET", "/v1/device/wallpapers", {})).status).toBe(401);
  });

  it("rejects bad images, someone else's Pocket, and a sixth wallpaper", async () => {
    expect((await call(app, "POST", "/v1/wallpapers", session, { name: "x", image_pbm_b64: "abc" })).body.error.code).toBe("bad_pbm");
    const other = getOrCreateUser("other@example.com");
    linkDevice(other.id, "dev-x", "tok-x");
    const notMine = await call(app, "POST", "/v1/wallpapers", session, { name: "x", image_pbm_b64: pbm(), devices: ["dev-x"] });
    expect(notMine.status).toBe(403);
    for (let i = 0; i < 5; i++) {
      expect((await call(app, "POST", "/v1/wallpapers", session, { name: `w${i}`, image_pbm_b64: pbm(i) })).status).toBe(201);
    }
    const sixth = await call(app, "POST", "/v1/wallpapers", session, { name: "w6", image_pbm_b64: pbm() });
    expect(sixth.status).toBe(409);
    expect(sixth.body.code).toBe("too_many");
    expect(sixth.body.wallpapers).toHaveLength(5);
  });

  it("renames, retargets and deletes", async () => {
    const wp = (await call(app, "POST", "/v1/wallpapers", session, { name: "One", image_pbm_b64: pbm() })).body.wallpaper;
    const patched = await call(app, "PATCH", `/v1/wallpapers/${wp.id}`, session, { name: "Two", devices: ["dev-b"] });
    expect(patched.body.wallpaper.name).toBe("Two");
    expect((await call(app, "GET", "/v1/device/wallpapers", dev)).body.wallpapers).toEqual([]);
    expect((await call(app, "GET", `/v1/wallpapers/${wp.id}/image.pbm`, session)).status).toBe(200);
    expect((await call(app, "DELETE", `/v1/wallpapers/${wp.id}`, session)).status).toBe(200);
    expect((await call(app, "GET", "/v1/wallpapers", session)).body.wallpapers).toEqual([]);
  });
});
