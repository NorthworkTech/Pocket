import { Hono } from "hono";
import type { AppEnv } from "../middleware/auth.js";
import { requireSession, requireEntitled } from "../middleware/auth.js";
import { get, run, all } from "../db/index.js";
import { newId } from "../lib/crypto.js";
import { AppError, Errors } from "../lib/errors.js";
import { LIST_ACCESS_SQL, displayName, familyIdForUser } from "../lib/family.js";
import {
  incomingWinsLww,
  normalizeUpdatedAt,
  mergeListItems,
  type ListItem,
} from "../lib/lww.js";

export type ListRow = {
  id: string;
  user_id: string;
  title: string;
  items_json: string;
  created_at: string;
  updated_at: string;
  updated_by_device_id: string;
  deleted_at: string | null;
  version: number;
  family_id?: string | null;
};

/** A list row joined with its owner's email (for family-shared lists). */
type ListRowWithOwner = ListRow & { owner_email?: string | null };

function parseItems(json: string): ListItem[] {
  try {
    const v = JSON.parse(json);
    return Array.isArray(v) ? v : [];
  } catch {
    return [];
  }
}

export function serializeList(row: ListRow) {
  return {
    id: row.id,
    title: row.title,
    items: parseItems(row.items_json),
    created_at: row.created_at,
    updated_at: row.updated_at,
    updated_by_device_id: row.updated_by_device_id,
    deleted_at: row.deleted_at,
    version: row.version,
  };
}

/** Companion shape: the sync fields plus who owns it and whether it's shared with the family. */
export function serializeListFor(row: ListRowWithOwner, viewerId: string) {
  const isOwner = row.user_id === viewerId;
  return {
    ...serializeList(row),
    shared: !!row.family_id,
    is_owner: isOwner,
    owner_name: displayName(row.owner_email),
  };
}

function accessibleList(id: string, userId: string): ListRowWithOwner | undefined {
  return get<ListRowWithOwner>(
    `SELECT l.*, u.email AS owner_email FROM cloud_lists l JOIN users u ON u.id = l.user_id
     WHERE l.id = ? AND ${LIST_ACCESS_SQL}`,
    [id, userId, userId],
  );
}

export const listsRoutes = new Hono<AppEnv>();

listsRoutes.use("*", requireSession, requireEntitled);

listsRoutes.get("/", (c) => {
  const user = c.get("user")!;
  const includeDeleted = c.req.query("include_deleted") === "1";
  // Own lists plus lists shared into the user's Pocket Family.
  const rows = all<ListRowWithOwner>(
    `SELECT l.*, u.email AS owner_email FROM cloud_lists l JOIN users u ON u.id = l.user_id
     WHERE ${LIST_ACCESS_SQL}${includeDeleted ? "" : " AND l.deleted_at IS NULL"}
     ORDER BY l.updated_at DESC`,
    [user.id, user.id],
  );
  return c.json({ lists: rows.map((r) => serializeListFor(r, user.id)) });
});

listsRoutes.post("/", async (c) => {
  const user = c.get("user")!;
  const body = await c.req.json().catch(() => ({}));
  const now = new Date();
  const id = typeof body.id === "string" && body.id ? body.id : newId();
  const existing = get<ListRow>("SELECT * FROM cloud_lists WHERE id = ?", [id]);

  const updated_at = normalizeUpdatedAt(
    typeof body.updated_at === "string" ? body.updated_at : undefined,
    now,
  );
  const updated_by =
    typeof body.updated_by_device_id === "string" && body.updated_by_device_id
      ? body.updated_by_device_id
      : "pwa";
  const title = typeof body.title === "string" ? body.title : "";
  const incomingItems: ListItem[] = Array.isArray(body.items) ? body.items : [];
  const deleted_at =
    body.deleted_at === null
      ? null
      : typeof body.deleted_at === "string"
        ? body.deleted_at
        : null;

  if (existing) {
    if (existing.user_id !== user.id) throw Errors.forbidden();
    if (
      !incomingWinsLww(
        { updated_at, updated_by_device_id: updated_by, deleted_at },
        {
          updated_at: existing.updated_at,
          updated_by_device_id: existing.updated_by_device_id,
          deleted_at: existing.deleted_at,
        },
      )
    ) {
      // Still merge items with per-item LWW if parent loses? Spec: parent LWW; if lose, keep existing.
      return c.json({ list: serializeList(existing), conflict: "kept_existing" });
    }
    const merged = mergeListItems(parseItems(existing.items_json), incomingItems);
    run(
      `UPDATE cloud_lists SET title = ?, items_json = ?, updated_at = ?, updated_by_device_id = ?, deleted_at = ?, version = version + 1
       WHERE id = ?`,
      [title, JSON.stringify(merged), updated_at, updated_by, deleted_at, id],
    );
  } else {
    run(
      `INSERT INTO cloud_lists
        (id, user_id, title, items_json, created_at, updated_at, updated_by_device_id, deleted_at, version)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, 1)`,
      [
        id,
        user.id,
        title,
        JSON.stringify(incomingItems),
        typeof body.created_at === "string" ? body.created_at : now.toISOString(),
        updated_at,
        updated_by,
        deleted_at,
      ],
    );
  }
  const row = get<ListRow>("SELECT * FROM cloud_lists WHERE id = ?", [id])!;
  return c.json({ list: serializeList(row) }, existing ? 200 : 201);
});

listsRoutes.get("/:id", (c) => {
  const user = c.get("user")!;
  const row = accessibleList(c.req.param("id"), user.id);
  if (!row || row.deleted_at) throw Errors.notFound("That list");
  return c.json({ list: serializeListFor(row, user.id) });
});

/** Share a list with (or take it back from) the owner's Pocket Family. Owner only. */
listsRoutes.post("/:id/share", async (c) => {
  const user = c.get("user")!;
  const id = c.req.param("id");
  const row = accessibleList(id, user.id);
  if (!row || row.deleted_at) throw Errors.notFound("That list");
  if (row.user_id !== user.id) {
    throw new AppError(403, "not_list_owner", "Only the person who made this list can change sharing.");
  }
  const body = await c.req.json().catch(() => ({}));
  if (typeof body?.shared !== "boolean") throw Errors.badRequest("Say whether to share the list.");
  let familyId: string | null = null;
  if (body.shared) {
    familyId = familyIdForUser(user.id);
    if (!familyId) throw Errors.badRequest("Start or join a Pocket Family first.");
  }
  run("UPDATE cloud_lists SET family_id = ? WHERE id = ?", [familyId, id]);
  return c.json({ list: serializeListFor(accessibleList(id, user.id)!, user.id) });
});

listsRoutes.patch("/:id", async (c) => {
  const user = c.get("user")!;
  const id = c.req.param("id");
  // Owner or a member of the family the list is shared with.
  const existing = accessibleList(id, user.id);
  if (!existing || existing.deleted_at) throw Errors.notFound("That list");

  const body = await c.req.json().catch(() => ({}));
  const now = new Date();
  const updated_at = normalizeUpdatedAt(
    typeof body.updated_at === "string" ? body.updated_at : now.toISOString(),
    now,
  );
  const updated_by =
    typeof body.updated_by_device_id === "string" && body.updated_by_device_id
      ? body.updated_by_device_id
      : "pwa";
  const title = typeof body.title === "string" ? body.title : existing.title;
  const incomingItems: ListItem[] = Array.isArray(body.items)
    ? body.items
    : parseItems(existing.items_json);

  if (
    !incomingWinsLww(
      { updated_at, updated_by_device_id: updated_by },
      {
        updated_at: existing.updated_at,
        updated_by_device_id: existing.updated_by_device_id,
        deleted_at: existing.deleted_at,
      },
    )
  ) {
    return c.json({ list: serializeListFor(existing, user.id), conflict: "kept_existing" });
  }

  const merged = mergeListItems(parseItems(existing.items_json), incomingItems);
  run(
    `UPDATE cloud_lists SET title = ?, items_json = ?, updated_at = ?, updated_by_device_id = ?, version = version + 1 WHERE id = ?`,
    [title, JSON.stringify(merged), updated_at, updated_by, id],
  );
  return c.json({ list: serializeListFor(accessibleList(id, user.id)!, user.id) });
});

listsRoutes.delete("/:id", async (c) => {
  const user = c.get("user")!;
  const id = c.req.param("id");
  const existing = accessibleList(id, user.id);
  if (!existing) throw Errors.notFound("That list");
  if (existing.user_id !== user.id) {
    throw new AppError(403, "not_list_owner", "Only the person who made this list can delete it.");
  }
  const now = new Date().toISOString();
  const body = await c.req.json().catch(() => ({}));
  const updated_by =
    typeof body.updated_by_device_id === "string" ? body.updated_by_device_id : "pwa";
  run(
    `UPDATE cloud_lists SET deleted_at = ?, updated_at = ?, updated_by_device_id = ?, version = version + 1 WHERE id = ?`,
    [now, now, updated_by, id],
  );
  return c.json({ ok: true, deleted_at: now });
});
