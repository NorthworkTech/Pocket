import { Hono } from "hono";
import type { AppEnv } from "../middleware/auth.js";
import { requireSession } from "../middleware/auth.js";
import { get, run } from "../db/index.js";
import { generatePairCode, isValidPairCode, newId, normalizePairCode } from "../lib/crypto.js";
import { AppError, Errors } from "../lib/errors.js";
import {
  FAMILY_CODE_TTL_MS,
  FAMILY_MAX_MEMBERS,
  parseFamilyName,
  serializeFamily,
  type FamilyRow,
} from "../lib/family.js";

/**
 * Pocket Family (Companion, session auth). Production mirror: neon-fn/scram-api.mjs.
 *   GET    /v1/family                     → { family | null }
 *   POST   /v1/family          {name}     → 201 { family }   (409 already in a family)
 *   PATCH  /v1/family          {name}     → { family }       (owner)
 *   DELETE /v1/family                     → { ok }           (owner; unshares every list)
 *   POST   /v1/family/code                → { family }       (owner; fresh 8-char code, 7 days)
 *   POST   /v1/family/join     {code}     → { family }       (404 bad code, 410 expired, 409 full/already in one)
 *   POST   /v1/family/leave               → { ok }           (members; owner deletes instead)
 *   DELETE /v1/family/members/:userId     → { family }       (owner removes someone else)
 */
export const familyRoutes = new Hono<AppEnv>();

familyRoutes.use("*", requireSession);

function myFamily(userId: string): FamilyRow | undefined {
  return get<FamilyRow>(
    `SELECT f.* FROM families f JOIN family_members m ON m.family_id = f.id WHERE m.user_id = ?`,
    [userId],
  );
}

function ownedFamily(userId: string): FamilyRow {
  const fam = myFamily(userId);
  if (!fam) throw Errors.notFound("Your family");
  if (fam.owner_user_id !== userId) {
    throw new AppError(403, "not_family_owner", "Only the family's owner can do that.");
  }
  return fam;
}

function freshCode(): { code: string; expires_at: string } {
  for (let i = 0; i < 5; i++) {
    const code = generatePairCode(8);
    if (!get("SELECT id FROM families WHERE join_code = ?", [code])) {
      return { code, expires_at: new Date(Date.now() + FAMILY_CODE_TTL_MS).toISOString() };
    }
  }
  throw Errors.conflict("Couldn't make a code. Try again.");
}

/** Lists a person shared into a family stop being shared once they're no longer in it. */
function unshareListsOf(userId: string, familyId: string): void {
  run("UPDATE cloud_lists SET family_id = NULL WHERE user_id = ? AND family_id = ?", [userId, familyId]);
}

familyRoutes.get("/", (c) => {
  const user = c.get("user")!;
  const fam = myFamily(user.id);
  return c.json({ family: fam ? serializeFamily(fam, user.id) : null });
});

familyRoutes.post("/", async (c) => {
  const user = c.get("user")!;
  const body = await c.req.json().catch(() => ({}));
  const parsed = parseFamilyName(body?.name);
  if (!parsed.ok) throw Errors.badRequest(parsed.message);
  if (myFamily(user.id)) {
    throw new AppError(409, "family_exists", "You're already in a Pocket Family.");
  }
  const id = newId();
  const now = new Date().toISOString();
  const { code, expires_at } = freshCode();
  run(
    `INSERT INTO families (id, name, owner_user_id, join_code, join_code_expires_at, created_at)
     VALUES (?, ?, ?, ?, ?, ?)`,
    [id, parsed.value, user.id, code, expires_at, now],
  );
  run("INSERT INTO family_members (user_id, family_id, role, joined_at) VALUES (?, ?, 'owner', ?)", [
    user.id,
    id,
    now,
  ]);
  return c.json({ family: serializeFamily(myFamily(user.id)!, user.id) }, 201);
});

familyRoutes.patch("/", async (c) => {
  const user = c.get("user")!;
  const fam = ownedFamily(user.id);
  const body = await c.req.json().catch(() => ({}));
  const parsed = parseFamilyName(body?.name);
  if (!parsed.ok) throw Errors.badRequest(parsed.message);
  run("UPDATE families SET name = ? WHERE id = ?", [parsed.value, fam.id]);
  return c.json({ family: serializeFamily(myFamily(user.id)!, user.id) });
});

familyRoutes.delete("/", (c) => {
  const user = c.get("user")!;
  const fam = ownedFamily(user.id);
  run("UPDATE cloud_lists SET family_id = NULL WHERE family_id = ?", [fam.id]);
  run("DELETE FROM family_members WHERE family_id = ?", [fam.id]);
  run("DELETE FROM families WHERE id = ?", [fam.id]);
  return c.json({ ok: true });
});

familyRoutes.post("/code", (c) => {
  const user = c.get("user")!;
  const fam = ownedFamily(user.id);
  const { code, expires_at } = freshCode();
  run("UPDATE families SET join_code = ?, join_code_expires_at = ? WHERE id = ?", [
    code,
    expires_at,
    fam.id,
  ]);
  return c.json({ family: serializeFamily(myFamily(user.id)!, user.id) });
});

familyRoutes.post("/join", async (c) => {
  const user = c.get("user")!;
  const body = await c.req.json().catch(() => ({}));
  const raw = typeof body?.code === "string" ? body.code : "";
  if (!isValidPairCode(raw)) throw new AppError(404, "family_code_invalid", "We couldn't find that code.");
  const code = normalizePairCode(raw);
  const fam = get<FamilyRow>("SELECT * FROM families WHERE join_code = ?", [code]);
  if (!fam) throw new AppError(404, "family_code_invalid", "We couldn't find that code.");
  if (!fam.join_code_expires_at || Date.parse(fam.join_code_expires_at) <= Date.now()) {
    throw new AppError(410, "family_code_expired", "This code has expired. Ask for a new one.");
  }
  const current = myFamily(user.id);
  if (current) {
    if (current.id === fam.id) return c.json({ family: serializeFamily(current, user.id) });
    throw new AppError(409, "family_exists", "You're already in a Pocket Family. Leave it first.");
  }
  const count = get<{ n: number }>("SELECT COUNT(*) AS n FROM family_members WHERE family_id = ?", [
    fam.id,
  ]);
  if (Number(count?.n ?? 0) >= FAMILY_MAX_MEMBERS) {
    throw new AppError(409, "family_full", `A Pocket Family can have up to ${FAMILY_MAX_MEMBERS} people.`);
  }
  run("INSERT INTO family_members (user_id, family_id, role, joined_at) VALUES (?, ?, 'member', ?)", [
    user.id,
    fam.id,
    new Date().toISOString(),
  ]);
  return c.json({ family: serializeFamily(fam, user.id) });
});

familyRoutes.post("/leave", (c) => {
  const user = c.get("user")!;
  const fam = myFamily(user.id);
  if (!fam) throw Errors.notFound("Your family");
  if (fam.owner_user_id === user.id) {
    throw Errors.badRequest("You own this family. Delete it instead, or remove people first.");
  }
  unshareListsOf(user.id, fam.id);
  run("DELETE FROM family_members WHERE user_id = ?", [user.id]);
  return c.json({ ok: true });
});

familyRoutes.delete("/members/:userId", (c) => {
  const user = c.get("user")!;
  const fam = ownedFamily(user.id);
  const target = c.req.param("userId");
  if (target === user.id) throw Errors.badRequest("You own this family. Delete it instead.");
  const member = get("SELECT user_id FROM family_members WHERE user_id = ? AND family_id = ?", [
    target,
    fam.id,
  ]);
  if (!member) throw Errors.notFound("That person");
  unshareListsOf(target, fam.id);
  run("DELETE FROM family_members WHERE user_id = ?", [target]);
  return c.json({ family: serializeFamily(fam, user.id) });
});
