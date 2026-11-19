/**
 * Pocket Family — one family per user, up to FAMILY_MAX_MEMBERS people, joined by a short code.
 * Lists can be shared into the owner's family (cloud_lists.family_id); any member can read and
 * edit a shared list, only its owner can unshare or delete it.
 * Production mirror: neon-fn/scram-api.mjs (keep both in step).
 */
import { all, get } from "../db/index.js";

export const FAMILY_MAX_MEMBERS = 6;
export const FAMILY_CODE_TTL_MS = 7 * 86400000;
export const FAMILY_NAME_MAX = 40;

/** Friendly name shown to other family members: the email's local part. */
export function displayName(email: string | null | undefined): string {
  const e = String(email || "");
  const at = e.indexOf("@");
  return (at > 0 ? e.slice(0, at) : e) || "Someone";
}

export function parseFamilyName(raw: unknown): { ok: true; value: string } | { ok: false; message: string } {
  const name = typeof raw === "string" ? raw.replace(/\s+/g, " ").trim() : "";
  if (!name) return { ok: false, message: "Give your family a name." };
  if (Array.from(name).length > FAMILY_NAME_MAX) {
    return { ok: false, message: `Family name must be ${FAMILY_NAME_MAX} characters or fewer.` };
  }
  return { ok: true, value: name };
}

export type FamilyRow = {
  id: string;
  name: string;
  owner_user_id: string;
  join_code: string | null;
  join_code_expires_at: string | null;
  created_at: string;
};

export function familyIdForUser(userId: string): string | null {
  const row = get<{ family_id: string }>("SELECT family_id FROM family_members WHERE user_id = ?", [
    userId,
  ]);
  return row?.family_id ?? null;
}

/**
 * SQL predicate (alias `l` = cloud_lists) for lists a user may read/edit: their own, or lists
 * shared into the family they belong to whose owner is still a member of that family.
 * Two bound params: [userId, userId].
 */
export const LIST_ACCESS_SQL = `(l.user_id = ? OR (l.family_id IS NOT NULL
  AND l.family_id = (SELECT fm.family_id FROM family_members fm WHERE fm.user_id = ?)
  AND EXISTS (SELECT 1 FROM family_members fo WHERE fo.user_id = l.user_id AND fo.family_id = l.family_id)))`;

/** Whether `userId` can see the list row (own or shared into their family). */
export function canAccessList(
  row: { user_id: string; family_id: string | null },
  userId: string,
): boolean {
  if (row.user_id === userId) return true;
  if (!row.family_id) return false;
  const mine = familyIdForUser(userId);
  if (!mine || mine !== row.family_id) return false;
  return familyIdForUser(row.user_id) === row.family_id;
}

export function serializeFamily(fam: FamilyRow, viewerId: string, now = new Date()) {
  const members = all<{ user_id: string; role: string; joined_at: string; email: string }>(
    `SELECT m.user_id, m.role, m.joined_at, u.email FROM family_members m
     JOIN users u ON u.id = m.user_id WHERE m.family_id = ? ORDER BY m.joined_at ASC`,
    [fam.id],
  );
  const isOwner = fam.owner_user_id === viewerId;
  const codeLive =
    !!fam.join_code &&
    !!fam.join_code_expires_at &&
    Date.parse(fam.join_code_expires_at) > now.getTime();
  return {
    id: fam.id,
    name: fam.name,
    role: isOwner ? "owner" : "member",
    created_at: fam.created_at,
    max_members: FAMILY_MAX_MEMBERS,
    // Only the owner invites, so only the owner sees the code.
    join_code: isOwner && codeLive ? fam.join_code : null,
    join_code_expires_at: isOwner && codeLive ? fam.join_code_expires_at : null,
    members: members.map((m) => ({
      user_id: m.user_id,
      name: displayName(m.email),
      role: m.role === "owner" ? "owner" : "member",
      joined_at: m.joined_at,
      is_me: m.user_id === viewerId,
    })),
  };
}
