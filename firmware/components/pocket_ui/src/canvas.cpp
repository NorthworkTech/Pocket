#include "pocket/canvas.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

extern "C" {
#include "qrcodegen.h"
}

namespace pocket {

namespace {

#include "font_ui.inc"

/** Decode one UTF-8 code point at `i` (advances i). Invalid bytes map to '?'. */
uint32_t next_cp(std::string_view s, size_t& i) {
  const unsigned char c = static_cast<unsigned char>(s[i++]);
  if (c < 0x80) return c;
  int extra = 0;
  uint32_t cp = 0;
  if ((c & 0xE0) == 0xC0) {
    extra = 1;
    cp = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    extra = 2;
    cp = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    extra = 3;
    cp = c & 0x07;
  } else {
    return '?';
  }
  for (int k = 0; k < extra; ++k) {
    if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return '?';
    cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
  }
  return cp;
}

const FontFace& face_for(Canvas::TextRole r) {
  switch (r) {
    case Canvas::TextRole::StatusBar:
      return kFontStatus;
    case Canvas::TextRole::Secondary:
      return kFontSecondary;
    case Canvas::TextRole::Caption:
      return kFontCaption;
    case Canvas::TextRole::BodyStrong:
      return kFontBodyStrong;
    case Canvas::TextRole::ScreenTitle:
    case Canvas::TextRole::WordMark:
      return kFontTitle;
    case Canvas::TextRole::PinDigit:
      return kFontPin;
    case Canvas::TextRole::Hero:
      return kFontHero;
    case Canvas::TextRole::Reading:
      return kFontReading;
    case Canvas::TextRole::HugeClock:
      return kFontHuge;
    case Canvas::TextRole::Body:
    default:
      return kFontBody;
  }
}

/** Faces with a reduced character set fall back to a full face of similar weight. */
const FontFace& fallback_for(Canvas::TextRole r) {
  switch (r) {
    case Canvas::TextRole::StatusBar:
      return kFontBodyStrong;
    case Canvas::TextRole::PinDigit:
    case Canvas::TextRole::Hero:
    case Canvas::TextRole::HugeClock:
      return kFontTitle;
    default:
      return kFontBody;
  }
}

const FontGlyph* find_glyph(const FontFace& f, uint32_t cp) {
  int lo = 0;
  int hi = static_cast<int>(f.count) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const uint32_t c = f.glyphs[mid].cp;
    if (c == cp) return &f.glyphs[mid];
    if (c < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}

/** Map a code point to a glyph + the face it came from (with ASCII stand-ins). */
const FontGlyph* resolve(Canvas::TextRole role, uint32_t cp, const FontFace** face_out) {
  const FontFace& f = face_for(role);
  if (const FontGlyph* g = find_glyph(f, cp)) {
    *face_out = &f;
    return g;
  }
  const FontFace& fb = fallback_for(role);
  if (const FontGlyph* g = find_glyph(fb, cp)) {
    *face_out = &fb;
    return g;
  }
  // Invisible formatting characters draw nothing (CR from Windows files, BOM, ZWSP).
  if (cp == '\r' || cp == 0xFEFF || cp == 0x200B || cp == 0x200D) return nullptr;
  // Typographic stand-ins the 1bpp fonts don't carry.
  if (cp == 0x2010 || cp == 0x2011 || cp == 0x2012 || cp == 0x2212) return resolve(role, '-', face_out);
  if (cp == 0x00A0 || cp == 0x2009 || cp == 0x202F || cp == 0x2007) return resolve(role, ' ', face_out);
  // Last resort: '?' in the primary face, else the fallback.
  if (const FontGlyph* g = find_glyph(f, '?')) {
    *face_out = &f;
    return g;
  }
  *face_out = &fb;
  return find_glyph(fb, '?');
}

}  // namespace

int Canvas::text_height(TextRole role) const { return face_for(role).line_h; }

int Canvas::text_width(std::string_view text, TextRole role) const {
  int w = 0;
  size_t i = 0;
  while (i < text.size()) {
    const uint32_t cp = next_cp(text, i);
    const FontFace* f = nullptr;
    if (const FontGlyph* g = resolve(role, cp, &f)) w += g->adv;
  }
  return w;
}

void Canvas::clear(Gray g) {
  const uint8_t v = static_cast<uint8_t>(g) & 0x3;
  const uint8_t byte = static_cast<uint8_t>((v << 6) | (v << 4) | (v << 2) | v);
  std::memset(buf_, byte, kBufBytes);
}

void Canvas::set_pixel(int x, int y, Gray g) {
  if (x < 0 || y < 0 || x >= kCanvasW || y >= kCanvasH) return;
  const size_t i = static_cast<size_t>(y * kCanvasW + x);
  const size_t bi = i / 4;
  const int shift = 6 - static_cast<int>((i % 4) * 2);
  buf_[bi] = static_cast<uint8_t>((buf_[bi] & ~(0x3 << shift)) | ((static_cast<uint8_t>(g) & 0x3) << shift));
}

Gray Canvas::get_pixel(int x, int y) const {
  if (x < 0 || y < 0 || x >= kCanvasW || y >= kCanvasH) return Gray::G3;
  const size_t i = static_cast<size_t>(y * kCanvasW + x);
  const size_t bi = i / 4;
  const int shift = 6 - static_cast<int>((i % 4) * 2);
  return static_cast<Gray>((buf_[bi] >> shift) & 0x3);
}

void Canvas::fill_rect(int x, int y, int w, int h, Gray g) {
  for (int yy = y; yy < y + h; ++yy)
    for (int xx = x; xx < x + w; ++xx) set_pixel(xx, yy, g);
}

void Canvas::stroke_rect(int x, int y, int w, int h, Gray g) {
  hline(x, y, w, g);
  hline(x, y + h - 1, w, g);
  vline(x, y, h, g);
  vline(x + w - 1, y, h, g);
}

void Canvas::fill_round_rect(int x, int y, int w, int h, int r, Gray g) {
  if (w <= 0 || h <= 0) return;
  r = std::max(0, std::min({r, w / 2, h / 2}));
  if (r <= 0) {
    fill_rect(x, y, w, h, g);
    return;
  }
  // Center + side bands (straight edges).
  fill_rect(x + r, y, w - 2 * r, h, g);
  fill_rect(x, y + r, r, h - 2 * r, g);
  fill_rect(x + w - r, y + r, r, h - 2 * r, g);
  // Quarter-circle corners.
  const int r2 = r * r;
  for (int dy = 0; dy < r; ++dy) {
    for (int dx = 0; dx < r; ++dx) {
      const int ox = r - 1 - dx;
      const int oy = r - 1 - dy;
      if (ox * ox + oy * oy > r2) continue;
      set_pixel(x + dx, y + dy, g);
      set_pixel(x + w - 1 - dx, y + dy, g);
      set_pixel(x + dx, y + h - 1 - dy, g);
      set_pixel(x + w - 1 - dx, y + h - 1 - dy, g);
    }
  }
}

void Canvas::stroke_round_rect(int x, int y, int w, int h, int r, Gray g, int thickness, Gray bg) {
  if (w <= 0 || h <= 0) return;
  thickness = std::max(1, thickness);
  r = std::max(0, std::min({r, w / 2, h / 2}));
  if (r <= 0) {
    for (int t = 0; t < thickness; ++t) {
      stroke_rect(x + t, y + t, w - 2 * t, h - 2 * t, g);
    }
    return;
  }
  // Paint outer round fill, then clear the interior back to the surrounding surface.
  fill_round_rect(x, y, w, h, r, g);
  const int inset = thickness;
  if (w > 2 * inset && h > 2 * inset) {
    const int ir = std::max(0, r - inset);
    fill_round_rect(x + inset, y + inset, w - 2 * inset, h - 2 * inset, ir, bg);
  }
}

void Canvas::hline(int x, int y, int w, Gray g) {
  for (int i = 0; i < w; ++i) set_pixel(x + i, y, g);
}

void Canvas::vline(int x, int y, int h, Gray g) {
  for (int i = 0; i < h; ++i) set_pixel(x, y + i, g);
}

void Canvas::line(int x0, int y0, int x1, int y1, Gray g) {
  int dx = std::abs(x1 - x0);
  int sx = x0 < x1 ? 1 : -1;
  int dy = -std::abs(y1 - y0);
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    set_pixel(x0, y0, g);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void Canvas::draw_text(int x, int y, std::string_view text, TextRole role, Gray g) {
  const FontFace& primary = face_for(role);
  int cx = x;
  size_t i = 0;
  while (i < text.size()) {
    const uint32_t cp = next_cp(text, i);
    const FontFace* f = nullptr;
    const FontGlyph* gl = resolve(role, cp, &f);
    if (!gl) continue;
    // Fallback faces have their own line box — re-center on the primary one.
    const int dy = (f == &primary) ? 0 : (primary.line_h - f->line_h) / 2;
    const uint8_t* bits = f->bits + gl->off;
    const int rb = (gl->w + 7) / 8;
    for (int row = 0; row < gl->h; ++row) {
      const uint8_t* r = bits + row * rb;
      const int py = y + dy + gl->yoff + row;
      for (int col = 0; col < gl->w; ++col) {
        if (r[col >> 3] & (0x80 >> (col & 7))) set_pixel(cx + gl->xoff + col, py, g);
      }
    }
    cx += gl->adv;
  }
}

void Canvas::draw_text_centered(int cx, int y, std::string_view text, TextRole role, Gray g) {
  const int w = text_width(text, role);
  draw_text(cx - w / 2, y, text, role, g);
}

void Canvas::draw_text_fit(int x, int y, int max_w, std::string_view text, TextRole role, Gray g) {
  if (text_width(text, role) <= max_w) {
    draw_text(x, y, text, role, g);
    return;
  }
  std::string s(text);
  const std::string ell = "\xE2\x80\xA6";  // …
  auto pop_cp = [&s]() {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
  };
  while (!s.empty() && text_width(s + ell, role) > max_w) pop_cp();
  while (!s.empty() && s.back() == ' ') s.pop_back();
  draw_text(x, y, s + ell, role, g);
}

int Canvas::draw_text_wrapped(int x, int y, int max_w, int line_gap, std::string_view text, TextRole role,
                              Gray g) {
  if (text.empty()) return y;
  if (max_w < 8) max_w = 8;
  const int line_h = text_height(role) + line_gap;
  int cy = y;
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i >= n) break;
    if (text[i] == '\n') {
      ++i;
      cy += line_h;
      continue;
    }
    size_t line_start = i;
    size_t last_break = i;
    size_t j = i;
    while (j < n && text[j] != '\n') {
      size_t word_end = j;
      while (word_end < n && text[word_end] != ' ' && text[word_end] != '\t' && text[word_end] != '\n') {
        ++word_end;
      }
      const std::string_view candidate = text.substr(line_start, word_end - line_start);
      if (text_width(candidate, role) <= max_w) {
        last_break = word_end;
        j = word_end;
        while (j < n && (text[j] == ' ' || text[j] == '\t')) ++j;
      } else {
        if (last_break == line_start) {
          // Single word longer than max_w — hard-split by characters.
          size_t cut = line_start + 1;
          while (cut < word_end && text_width(text.substr(line_start, cut - line_start), role) <= max_w) {
            ++cut;
          }
          if (cut > line_start + 1) --cut;
          draw_text(x, cy, text.substr(line_start, cut - line_start), role, g);
          cy += line_h;
          line_start = cut;
          last_break = cut;
          j = cut;
        } else {
          break;
        }
      }
    }
    if (last_break > line_start) {
      // Trim trailing spaces on the emitted line.
      size_t end = last_break;
      while (end > line_start && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
      if (end > line_start) {
        draw_text(x, cy, text.substr(line_start, end - line_start), role, g);
      }
      cy += line_h;
      i = last_break;
      while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r')) ++i;
      if (i < n && text[i] == '\n') ++i;  // the newline ends this line, not a blank one
    } else if (j < n && text[j] == '\n') {
      i = j + 1;
      cy += line_h;
    } else {
      break;
    }
  }
  return cy;
}

void Canvas::draw_focus_tile(int x, int y, int w, int h, std::string_view label, TextRole role) {
  fill_round_rect(x, y, w, h, 12, Gray::G0);
  const int th = text_height(role);
  // Same inset as unfocused rows (kRowLabelInset) so labels never jump on focus.
  draw_text_fit(x + kRowLabelInset, y + (h - th) / 2, std::max(8, w - 2 * kRowLabelInset), label, role,
                Gray::G3);
}

void Canvas::dotted_hline(int x, int y, int w, Gray g, int step) {
  for (int i = 0; i < w; i += step) set_pixel(x + i, y, g);
}

namespace {
constexpr uint8_t kBayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
}

void Canvas::fill_tone(int x, int y, int w, int h, int level) {
  if (level <= 0) {
    fill_rect(x, y, w, h, Gray::G3);
    return;
  }
  for (int yy = y; yy < y + h; ++yy)
    for (int xx = x; xx < x + w; ++xx)
      set_pixel(xx, yy, kBayer4[yy & 3][xx & 3] < level ? Gray::G0 : Gray::G3);
}

void Canvas::fill_disc(int cx, int cy, int r, Gray g) {
  for (int dy = -r; dy <= r; ++dy) {
    const int hw = static_cast<int>(std::sqrt(static_cast<float>(r * r - dy * dy)) + 0.5f);
    hline(cx - hw, cy + dy, 2 * hw + 1, g);
  }
}

void Canvas::stroke_circle(int cx, int cy, int r, Gray g, int thickness) {
  const int ro2 = r * r;
  const int ri = std::max(0, r - thickness);
  const int ri2 = ri * ri;
  for (int dy = -r; dy <= r; ++dy)
    for (int dx = -r; dx <= r; ++dx) {
      const int d2 = dx * dx + dy * dy;
      if (d2 <= ro2 + r / 2 && d2 > ri2 + ri / 2) set_pixel(cx + dx, cy + dy, g);
    }
}

void Canvas::thick_line(int x0, int y0, int x1, int y1, int thickness, Gray g) {
  const int r = std::max(0, thickness / 2);
  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int steps = std::max(1, std::max(std::abs(dx), std::abs(dy)));
  for (int s = 0; s <= steps; ++s) {
    const int x = x0 + dx * s / steps;
    const int y = y0 + dy * s / steps;
    if (r == 0) set_pixel(x, y, g);
    else fill_disc(x, y, r, g);
  }
}

bool Canvas::draw_qr(int x, int y, int size, std::string_view payload) {
  uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
  uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
  std::string data(payload);
  if (!qrcodegen_encodeText(data.c_str(), temp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                            qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
    return false;
  }
  const int n = qrcodegen_getSize(qr);
  if (n <= 0) return false;
  const int cell = std::max(1, size / n);
  const int drawn = cell * n;
  const int ox = x + (size - drawn) / 2;
  const int oy = y + (size - drawn) / 2;
  fill_rect(ox, oy, drawn, drawn, Gray::G3);
  for (int r = 0; r < n; ++r) {
    for (int c = 0; c < n; ++c) {
      if (qrcodegen_getModule(qr, c, r)) fill_rect(ox + c * cell, oy + r * cell, cell, cell, Gray::G0);
    }
  }
  return true;
}

}  // namespace pocket
