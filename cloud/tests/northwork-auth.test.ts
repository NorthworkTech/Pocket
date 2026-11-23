import { createHmac } from "node:crypto";
import { describe, expect, it } from "vitest";
import { checkNorthworkPass, verifyNorthworkPass } from "../src/lib/auth.js";

function pass(secret: string, payload: object): string {
  const body = Buffer.from(JSON.stringify(payload)).toString("base64url");
  return `${body}.${createHmac("sha256", secret).update(body).digest("base64url")}`;
}

describe("Continue with Northwork pass", () => {
  const secret = "test-secret";
  const ok = { sub: "u1", email: "Hi@Example.com", name: "Hi", aud: "pocket", exp: Date.now() + 60_000 };

  it("accepts a fresh pass signed with the shared secret", () => {
    expect(verifyNorthworkPass(pass(secret, ok), secret)?.email).toBe("Hi@Example.com");
  });
  it("rejects a wrong signature, wrong audience, expiry, missing secret", () => {
    expect(verifyNorthworkPass(pass("other", ok), secret)).toBeNull();
    expect(verifyNorthworkPass(pass(secret, { ...ok, aud: "community" }), secret)).toBeNull();
    expect(verifyNorthworkPass(pass(secret, { ...ok, exp: Date.now() - 1 }), secret)).toBeNull();
    expect(verifyNorthworkPass(pass(secret, ok), "")).toBeNull();
    expect(verifyNorthworkPass("garbage", secret)).toBeNull();
  });
  it("matches the site's signing format (b64url body + b64url HMAC)", () => {
    const t = pass(secret, ok);
    expect(t.split(".")).toHaveLength(2);
    expect(t).not.toMatch(/[+/=]/);
  });
});

describe("checkNorthworkPass without a local secret", () => {
  it("asks northwork.app and trusts only an ok answer", async () => {
    const calls: string[] = [];
    const good = (async (url: string) => {
      calls.push(url);
      return new Response(JSON.stringify({ ok: true, user: { id: "u9", email: "a@b.co" } }), { status: 200 });
    }) as unknown as typeof fetch;
    const bad = (async () => new Response("{}", { status: 401 })) as unknown as typeof fetch;
    expect((await checkNorthworkPass("x.y", "", "https://northwork.app", good))?.email).toBe("a@b.co");
    expect(calls[0]).toBe("https://northwork.app/api/integrations/pocket/verify");
    expect(await checkNorthworkPass("x.y", "", "https://northwork.app", bad)).toBeNull();
    expect(await checkNorthworkPass(123, "", "https://northwork.app", good)).toBeNull();
  });
});
