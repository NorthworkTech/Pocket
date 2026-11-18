import { Hono } from "hono";
import type { AppEnv } from "../middleware/auth.js";
import { requireDeviceToken, requireEntitled, requireSession } from "../middleware/auth.js";
import { all, run } from "../db/index.js";
import { Errors } from "../lib/errors.js";
import { parseReadingProgress } from "../lib/device-features.js";

/**
 * Reading — Companion library list (with per-book progress) and the device's progress report.
 * Production mirror (incl. upload/text/delete): neon-fn/scram-api.mjs.
 */

type BookRow = {
  id: string;
  title: string;
  author: string;
  format: string;
  filename: string;
  size_bytes: number;
  created_at: string;
};

type ProgressRow = {
  book_id: string;
  page: number;
  pages: number;
  percent: number;
  updated_at: string;
};

export function serializeProgress(p: ProgressRow | undefined) {
  if (!p) return null;
  return {
    percent: Number(p.percent),
    page: Number(p.page),
    pages: Number(p.pages),
    updated_at: p.updated_at,
  };
}

export const bookRoutes = new Hono<AppEnv>();

bookRoutes.get("/", requireSession, requireEntitled, async (c) => {
  const user = c.get("user")!;
  const rows = all<BookRow>(
    `SELECT id, title, author, format, filename, size_bytes, created_at FROM books
     WHERE user_id = ? AND deleted_at IS NULL ORDER BY created_at DESC`,
    [user.id],
  );
  const progress = new Map(
    all<ProgressRow>(
      "SELECT book_id, page, pages, percent, updated_at FROM reading_progress WHERE user_id = ?",
      [user.id],
    ).map((p) => [p.book_id, p]),
  );
  return c.json({
    books: rows.map((r) => ({
      id: r.id,
      title: r.title || "",
      author: r.author || "",
      format: r.format || "txt",
      filename: r.filename || "",
      size: Number(r.size_bytes || 0),
      size_bytes: Number(r.size_bytes || 0),
      created_at: r.created_at,
      progress: serializeProgress(progress.get(r.id)),
    })),
  });
});

export const deviceReadingRoutes = new Hono<AppEnv>();

deviceReadingRoutes.post("/reading", requireDeviceToken, async (c) => {
  const user = c.get("user")!;
  const link = c.get("deviceLink")!;
  const body = await c.req.json().catch(() => null);
  const parsed = parseReadingProgress(body);
  if (!parsed.ok) throw Errors.badRequest(parsed.message);
  const p = parsed.value;
  // Unknown book ids are stored anyway: the device may report before the library list catches up,
  // and /v1/books only surfaces progress for books that exist.
  run(
    `INSERT INTO reading_progress (user_id, book_id, device_id, page, pages, percent, updated_at)
     VALUES (?, ?, ?, ?, ?, ?, ?)
     ON CONFLICT (user_id, book_id) DO UPDATE SET device_id = excluded.device_id,
       page = excluded.page, pages = excluded.pages, percent = excluded.percent,
       updated_at = excluded.updated_at`,
    [user.id, p.book_id, link.device_id, p.page, p.pages, p.percent, new Date().toISOString()],
  );
  return c.json({ ok: true });
});
