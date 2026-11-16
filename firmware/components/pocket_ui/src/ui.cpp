#include "pocket/ui.hpp"
#include "pocket/grayscale.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pocket {
namespace ui {
namespace {

constexpr float kPi = 3.14159265f;

inline Gray inverse(Gray g) { return g == Gray::G0 ? Gray::G3 : Gray::G0; }

/** Stroke pen on a 24-unit icon grid. */
struct Pen {
  Canvas& c;
  float ox, oy, s;
  int w;
  Gray g;

  int X(float u) const { return static_cast<int>(std::lround(ox + u * s)); }
  int Y(float v) const { return static_cast<int>(std::lround(oy + v * s)); }

  void dot(int x, int y, Gray col) const {
    const int r = w / 2;
    if (r <= 0) {
      c.set_pixel(x, y, col);
      return;
    }
    c.fill_disc(x, y, r, col);
  }
  void line(float x0, float y0, float x1, float y1) const { line_c(x0, y0, x1, y1, g); }
  void line_c(float x0, float y0, float x1, float y1, Gray col) const {
    const float dx = (x1 - x0) * s, dy = (y1 - y0) * s;
    const int steps = std::max(1, static_cast<int>(std::max(std::fabs(dx), std::fabs(dy))));
    for (int i = 0; i <= steps; ++i) {
      const float t = static_cast<float>(i) / steps;
      dot(X(x0 + (x1 - x0) * t), Y(y0 + (y1 - y0) * t), col);
    }
  }
  /** Arc from a0 to a1 (radians, 0 = +x, clockwise on screen since y grows down). */
  void arc(float cx, float cy, float r, float a0, float a1) const {
    const int steps = std::max(8, static_cast<int>(std::fabs(a1 - a0) * r * s));
    for (int i = 0; i <= steps; ++i) {
      const float a = a0 + (a1 - a0) * i / steps;
      dot(X(cx + r * std::cos(a)), Y(cy + r * std::sin(a)), g);
    }
  }
  void circle(float cx, float cy, float r) const { arc(cx, cy, r, 0, 2 * kPi); }
  void rrect(float x, float y, float w_, float h_, float r) const {
    line(x + r, y, x + w_ - r, y);
    line(x + r, y + h_, x + w_ - r, y + h_);
    line(x, y + r, x, y + h_ - r);
    line(x + w_, y + r, x + w_, y + h_ - r);
    arc(x + r, y + r, r, kPi, 1.5f * kPi);
    arc(x + w_ - r, y + r, r, 1.5f * kPi, 2 * kPi);
    arc(x + w_ - r, y + h_ - r, r, 0, 0.5f * kPi);
    arc(x + r, y + h_ - r, r, 0.5f * kPi, kPi);
  }
  void disc(float cx, float cy, float r, Gray col) const {
    c.fill_disc(X(cx), Y(cy), std::max(1, static_cast<int>(std::lround(r * s))), col);
  }
  void tri(float x0, float y0, float x1, float y1, float x2, float y2) const {
    const int ax = X(x0), ay = Y(y0), bx = X(x1), by = Y(y1), cx = X(x2), cy = Y(y2);
    const int miny = std::min({ay, by, cy}), maxy = std::max({ay, by, cy});
    for (int y = miny; y <= maxy; ++y) {
      float xs[3];
      int n = 0;
      auto edge = [&](int x0e, int y0e, int x1e, int y1e) {
        if ((y < y0e) == (y < y1e) || y0e == y1e) return;
        xs[n++] = x0e + static_cast<float>(y - y0e) * (x1e - x0e) / (y1e - y0e);
      };
      edge(ax, ay, bx, by);
      edge(bx, by, cx, cy);
      edge(cx, cy, ax, ay);
      if (n >= 2) {
        const int l = static_cast<int>(std::min(xs[0], xs[1]));
        const int r = static_cast<int>(std::max(xs[0], xs[1]));
        c.hline(l, y, r - l + 1, g);
      }
    }
  }
  void box(float x, float y, float w_, float h_, Gray col) const {
    c.fill_rect(X(x), Y(y), X(x + w_) - X(x), Y(y + h_) - Y(y), col);
  }
};

void draw_icon(const Pen& p, Icon icon) {
  const Gray bg = inverse(p.g);
  switch (icon) {
    case Icon::Notes:
      p.rrect(5, 2.5f, 14, 19, 2.5f);
      p.line(8.5f, 8, 15.5f, 8);
      p.line(8.5f, 12, 15.5f, 12);
      p.line(8.5f, 16, 12.5f, 16);
      break;
    case Icon::Ledger:
      p.rrect(4.5f, 2.5f, 15, 19, 2.5f);
      p.line(8.5f, 2.5f, 8.5f, 21.5f);
      p.line(12, 8, 16, 8);
      p.line(12, 12, 16, 12);
      break;
    case Icon::Clock:
      p.circle(12, 12, 9.5f);
      p.line(12, 12, 12, 6.5f);
      p.line(12, 12, 15.5f, 14);
      break;
    case Icon::Alarm:
      p.circle(12, 13, 8);
      p.line(12, 13, 12, 9);
      p.line(12, 13, 14.5f, 14.5f);
      p.line(3.5f, 5.5f, 6.5f, 3);
      p.line(20.5f, 5.5f, 17.5f, 3);
      break;
    case Icon::Timer:
      p.line(7, 2.5f, 17, 2.5f);
      p.line(7, 21.5f, 17, 21.5f);
      p.line(8, 2.5f, 16, 21.5f);
      p.line(16, 2.5f, 8, 21.5f);
      break;
    case Icon::Pass:
      p.rrect(2.5f, 6, 19, 12, 2.5f);
      p.disc(2.5f, 12, 2.6f, bg);
      p.disc(21.5f, 12, 2.6f, bg);
      for (float y = 8.5f; y <= 15.6f; y += 2.4f) p.disc(15, y, 0.7f, p.g);
      p.line(6.5f, 10.5f, 11, 10.5f);
      p.line(6.5f, 13.5f, 9.5f, 13.5f);
      break;
    case Icon::Weather:
      p.circle(12, 12, 4.2f);
      for (int i = 0; i < 8; ++i) {
        const float a = i * kPi / 4;
        p.line(12 + 7 * std::cos(a), 12 + 7 * std::sin(a), 12 + 9.5f * std::cos(a), 12 + 9.5f * std::sin(a));
      }
      break;
    case Icon::Cloud:
    case Icon::Rain: {
      const float yo = icon == Icon::Rain ? -2.5f : 0;
      p.arc(8.5f, 14 + yo, 4, 0.5f * kPi, 1.5f * kPi);
      p.arc(13, 11 + yo, 5.5f, 1.1f * kPi, 1.95f * kPi);
      p.arc(17, 14.5f + yo, 3.5f, 1.5f * kPi, 2.5f * kPi);
      p.line(8.5f, 18 + yo, 17, 18 + yo);
      if (icon == Icon::Rain) {
        p.line(8, 19, 7, 22);
        p.line(12, 19, 11, 22);
        p.line(16, 19, 15, 22);
      }
      break;
    }
    case Icon::Music:
      p.line(9.5f, 17, 9.5f, 5);
      p.line(9.5f, 5, 19, 3);
      p.line(19, 3, 19, 15);
      p.line(9.5f, 8.5f, 19, 6.5f);
      p.disc(7, 17, 2.8f, p.g);
      p.disc(16.5f, 15, 2.8f, p.g);
      break;
    case Icon::Reading:
      p.line(12, 6, 12, 20);
      p.line(12, 6, 3.5f, 4.5f);
      p.line(3.5f, 4.5f, 3.5f, 18);
      p.line(3.5f, 18, 12, 20);
      p.line(12, 6, 20.5f, 4.5f);
      p.line(20.5f, 4.5f, 20.5f, 18);
      p.line(20.5f, 18, 12, 20);
      break;
    case Icon::Settings: {
      // Three sliders — "adjust", calmer than a gear at e-ink sizes.
      const float ys[3] = {6, 12, 18};
      const float ks[3] = {15.5f, 8, 13};
      for (int i = 0; i < 3; ++i) {
        p.line(3.5f, ys[i], 20.5f, ys[i]);
        p.disc(ks[i], ys[i], 3.0f, p.g);
        p.disc(ks[i], ys[i], 1.3f, bg);
      }
      break;
    }
    case Icon::Update:
    {
      // Clockwise ring with a filled arrowhead at its end.
      const float a0 = -0.30f * kPi, a1 = 1.30f * kPi, r = 8;
      p.arc(12, 12, r, a0, a1);
      const float ex = 12 + r * std::cos(a1), ey = 12 + r * std::sin(a1);
      const float tx = -std::sin(a1), ty = std::cos(a1);  // tangent (clockwise)
      const float nx = std::cos(a1), ny = std::sin(a1);
      p.tri(ex + 4.5f * tx, ey + 4.5f * ty, ex + 3.6f * nx - 1.0f * tx, ey + 3.6f * ny - 1.0f * ty,
            ex - 3.6f * nx - 1.0f * tx, ey - 3.6f * ny - 1.0f * ty);
    }
      break;
    case Icon::ChevronRight:
      p.line(9, 5, 16, 12);
      p.line(16, 12, 9, 19);
      break;
    case Icon::ChevronLeft:
      p.line(15, 5, 8, 12);
      p.line(8, 12, 15, 19);
      break;
    case Icon::Check:
      p.line(4.5f, 12.5f, 9.5f, 17.5f);
      p.line(9.5f, 17.5f, 19.5f, 6.5f);
      break;
    case Icon::Play:
      p.tri(7, 4, 7, 20, 20, 12);
      break;
    case Icon::Pause:
      p.box(6, 4.5f, 4, 15, p.g);
      p.box(14, 4.5f, 4, 15, p.g);
      break;
    case Icon::Stop:
      p.box(5.5f, 5.5f, 13, 13, p.g);
      break;
    case Icon::Plus:
      p.line(12, 4.5f, 12, 19.5f);
      p.line(4.5f, 12, 19.5f, 12);
      break;
    case Icon::Mic:
      p.rrect(8.5f, 2.5f, 7, 12, 3.5f);
      p.arc(12, 11, 7, 0.05f * kPi, 0.95f * kPi);
      p.line(12, 18, 12, 21.5f);
      p.line(8.5f, 21.5f, 15.5f, 21.5f);
      break;
    case Icon::Wifi:
      p.arc(12, 19, 15, 1.25f * kPi, 1.75f * kPi);
      p.arc(12, 19, 10, 1.25f * kPi, 1.75f * kPi);
      p.arc(12, 19, 5, 1.25f * kPi, 1.75f * kPi);
      p.disc(12, 19, 1.6f, p.g);
      break;
    case Icon::Lock:
      p.rrect(5, 10.5f, 14, 11, 2);
      p.arc(12, 9.5f, 4.5f, kPi, 2 * kPi);
      p.line(7.5f, 9.5f, 7.5f, 10.5f);
      p.line(16.5f, 9.5f, 16.5f, 10.5f);
      p.disc(12, 16, 1.4f, p.g);
      break;
    case Icon::Trash:
      p.line(4, 6, 20, 6);
      p.line(9.5f, 6, 10, 3);
      p.line(10, 3, 14, 3);
      p.line(14, 3, 14.5f, 6);
      p.line(6, 6, 7, 21);
      p.line(7, 21, 17, 21);
      p.line(17, 21, 18, 6);
      p.line(10.5f, 10, 10.5f, 17);
      p.line(13.5f, 10, 13.5f, 17);
      break;
    case Icon::Sd:
      p.line(6, 2.5f, 15, 2.5f);
      p.line(15, 2.5f, 19, 6.5f);
      p.line(19, 6.5f, 19, 21.5f);
      p.line(19, 21.5f, 6, 21.5f);
      p.line(6, 21.5f, 6, 2.5f);
      p.line(9.5f, 5.5f, 9.5f, 8.5f);
      p.line(12.5f, 5.5f, 12.5f, 8.5f);
      p.line(15.5f, 7, 15.5f, 8.5f);
      break;
    case Icon::Info:
      p.circle(12, 12, 9.5f);
      p.line(12, 11, 12, 17);
      p.disc(12, 7.5f, 1.4f, p.g);
      break;
    case Icon::Phone:
      p.rrect(6.5f, 2.5f, 11, 19, 2.5f);
      p.line(10.5f, 18.5f, 13.5f, 18.5f);
      break;
    case Icon::Sound:
      p.tri(3, 9, 3, 15, 8, 15);
      p.tri(3, 9, 8, 9, 8, 15);
      p.tri(8, 9, 13, 4, 13, 20);
      p.tri(8, 9, 13, 20, 8, 15);
      p.arc(13, 12, 4.5f, -0.3f * kPi, 0.3f * kPi);
      p.arc(13, 12, 8, -0.3f * kPi, 0.3f * kPi);
      break;
    case Icon::Display:
      p.rrect(3, 3.5f, 18, 13, 2);
      p.line(12, 16.5f, 12, 20);
      p.line(8, 20.5f, 16, 20.5f);
      break;
    case Icon::Home:
      p.line(3.5f, 11.5f, 12, 4);
      p.line(12, 4, 20.5f, 11.5f);
      p.line(6, 9.5f, 6, 20);
      p.line(18, 9.5f, 18, 20);
      p.line(6, 20, 18, 20);
      break;
  }
}

std::string upper_spaced(std::string_view s) {
  std::string out;
  for (char ch : s) {
    char u = (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 32) : ch;
    out += u;
  }
  return out;
}

/** Draw text with extra tracking (letter-spacing) — used for small caps labels. */
void draw_tracked(Canvas& c, int x, int y, std::string_view s, Canvas::TextRole role, Gray g, int track) {
  char buf[2] = {0, 0};
  for (char ch : s) {
    buf[0] = ch;
    c.draw_text(x, y, buf, role, g);
    x += c.text_width(buf, role) + track;
  }
}

int tracked_width(Canvas& c, std::string_view s, Canvas::TextRole role, int track) {
  char buf[2] = {0, 0};
  int w = 0;
  for (char ch : s) {
    buf[0] = ch;
    w += c.text_width(buf, role) + track;
  }
  return w - (s.empty() ? 0 : track);
}

/** Word-wrap into lines no wider than max_w (UTF-8 safe on spaces). */
std::vector<std::string> wrap(Canvas& c, std::string_view text, Canvas::TextRole role, int max_w) {
  std::vector<std::string> lines;
  std::string cur;
  size_t i = 0;
  while (i < text.size()) {
    size_t j = text.find(' ', i);
    if (j == std::string_view::npos) j = text.size();
    std::string word(text.substr(i, j - i));
    std::string trial = cur.empty() ? word : cur + " " + word;
    if (!cur.empty() && c.text_width(trial, role) > max_w) {
      lines.push_back(cur);
      cur = word;
    } else {
      cur = trial;
    }
    i = j + 1;
  }
  if (!cur.empty()) lines.push_back(cur);
  return lines;
}

}  // namespace

void icon(Canvas& c, Icon ic, int cx, int cy, int size, Gray g) {
  const float s = size / 24.0f;
  const int w = std::max(2, static_cast<int>(std::lround(size / 13.0f)));
  Pen p{c, cx - size / 2.0f, cy - size / 2.0f, s, w, g};
  draw_icon(p, ic);
}

int header(Canvas& c, std::string_view title, bool back) {
  int x = kSideMargin;
  const int th = c.text_height(Canvas::TextRole::ScreenTitle);
  if (back) {
    icon(c, Icon::ChevronLeft, kSideMargin + 9, kContentTop + th / 2 + 1, 24, Gray::G0);
    x += 28;
  }
  c.draw_text_fit(x, kContentTop, kCanvasW - kSideMargin - x, title, Canvas::TextRole::ScreenTitle, Gray::G0);
  return kListTop;
}

void section(Canvas& c, int y, std::string_view label) {
  const std::string up = upper_spaced(label);
  draw_tracked(c, kSideMargin, y, up, Canvas::TextRole::Caption, Gray::G0, 2);
  const int w = tracked_width(c, up, Canvas::TextRole::Caption, 2);
  const int ry = y + c.text_height(Canvas::TextRole::Caption) / 2 + 1;
  c.dotted_hline(kSideMargin + w + 12, ry, kCanvasW - kSideMargin - (kSideMargin + w + 12), Gray::G0, 3);
}

void row_rule(Canvas& c, int y) {
  c.dotted_hline(kSideMargin + kRowLabelInset, y + kFocusRowH + (kRowPitch - kFocusRowH) / 2,
                 kFocusRowW - 2 * kRowLabelInset, Gray::G0, 3);
}

void row(Canvas& c, int y, const Row& r, bool focused) {
  const int x = kSideMargin;
  const int w = kFocusRowW;
  const int h = kFocusRowH;
  const Gray ink = focused ? Gray::G3 : Gray::G0;
  const Gray bg = focused ? Gray::G0 : Gray::G3;
  if (focused) c.fill_round_rect(x, y, w, h, 12, Gray::G0);
  else if (r.kind != RowKind::Back) row_rule(c, y);
  int lx = x + kRowLabelInset;
  int rx = x + w - kRowLabelInset;  // right edge for trailing content
  const int cy = y + h / 2;

  // Leading glyph.
  switch (r.kind) {
    case RowKind::Back:
      icon(c, Icon::ChevronLeft, lx + 8, cy, 22, ink);
      lx += 28;
      break;
    case RowKind::Check:
      if (r.on) {
        c.fill_round_rect(lx, cy - 12, 24, 24, 5, ink);
        icon(c, Icon::Check, lx + 12, cy, 20, bg);
      } else {
        c.stroke_round_rect(lx, cy - 12, 24, 24, 5, ink, 2, bg);
      }
      lx += 38;
      break;
    case RowKind::Radio:
      c.stroke_circle(lx + 12, cy, 12, ink, 2);
      if (r.on) c.fill_disc(lx + 12, cy, 6, ink);
      lx += 38;
      break;
    default:
      if (r.has_lead) {
        icon(c, r.lead, lx + 12, cy, 24, ink);
        lx += 40;
      }
      break;
  }

  // Trailing content.
  switch (r.kind) {
    case RowKind::Nav:
      icon(c, Icon::ChevronRight, rx - 9, cy, 20, ink);
      rx -= 30;
      if (!r.value.empty()) {
        const int vw = std::min(c.text_width(r.value, Canvas::TextRole::Secondary), (rx - lx) / 2);
        c.draw_text_fit(rx - vw, cy - c.text_height(Canvas::TextRole::Secondary) / 2, vw, r.value,
                        Canvas::TextRole::Secondary, ink);
        rx -= vw + 12;
      }
      break;
    case RowKind::Value: {
      const int vw = std::min(c.text_width(r.value, Canvas::TextRole::Secondary), (rx - lx) * 3 / 5);
      c.draw_text_fit(rx - vw, cy - c.text_height(Canvas::TextRole::Secondary) / 2, vw, r.value,
                      Canvas::TextRole::Secondary, ink);
      rx -= vw + 14;
      break;
    }
    case RowKind::Toggle: {
      constexpr int kSw = 46, kSh = 26;
      const int sx = rx - kSw, sy = cy - kSh / 2;
      if (r.on) {
        c.fill_round_rect(sx, sy, kSw, kSh, kSh / 2, ink);
        c.fill_disc(sx + kSw - kSh / 2, cy, kSh / 2 - 4, bg);
      } else {
        c.stroke_round_rect(sx, sy, kSw, kSh, kSh / 2, ink, 2, bg);
        c.fill_disc(sx + kSh / 2, cy, kSh / 2 - 5, ink);
      }
      rx = sx - 14;
      break;
    }
    default:
      break;
  }

  const Canvas::TextRole role = r.kind == RowKind::Action ? Canvas::TextRole::BodyStrong : Canvas::TextRole::Body;
  const int ty = y + (h - c.text_height(role)) / 2;
  c.draw_text_fit(lx, ty, std::max(8, rx - lx), r.label, role, ink);
  if (r.kind == RowKind::Check && r.on) {
    const int tw = std::min(c.text_width(r.label, role), rx - lx);
    c.hline(lx, ty + 18, tw, ink);
    c.hline(lx, ty + 19, tw, ink);
  }
}

void auto_row(Canvas& c, int y, std::string_view text, bool focused) {
  Row r;
  if (text == "Back") {
    r.label = "Back";
    r.kind = RowKind::Back;
    row(c, y, r, focused);
    return;
  }
  const size_t colon = text.rfind(": ");
  if (colon != std::string_view::npos) {
    r.label = text.substr(0, colon);
    r.value = text.substr(colon + 2);
    if (r.value == "On" || r.value == "Off") {
      r.kind = RowKind::Toggle;
      r.on = r.value == "On";
    } else {
      r.kind = RowKind::Value;
    }
  } else {
    r.label = text;
  }
  row(c, y, r, focused);
}

void tabs(Canvas& c, int y, const char* const* labels, int n, int active) {
  if (n <= 0) return;
  const int x = kSideMargin;
  const int w = kFocusRowW;
  const int h = 44;
  const int ty = y + (kFocusRowH - h) / 2;
  c.stroke_round_rect(x, ty, w, h, h / 2, Gray::G0, 2, Gray::G3);
  const int seg = (w - 8) / n;
  for (int i = 0; i < n; ++i) {
    const int sx = x + 4 + i * seg;
    const bool on = i == active;
    if (on) c.fill_round_rect(sx, ty + 4, seg, h - 8, (h - 8) / 2, Gray::G0);
    const Canvas::TextRole role = on ? Canvas::TextRole::BodyStrong : Canvas::TextRole::Secondary;
    const int lw = c.text_width(labels[i], role);
    c.draw_text(sx + (seg - lw) / 2, ty + (h - c.text_height(role)) / 2, labels[i], role,
                on ? Gray::G3 : Gray::G0);
  }
}

void button(Canvas& c, int y, std::string_view label, bool focused, Icon lead, bool has_lead) {
  const int x = kSideMargin;
  const int w = kFocusRowW;
  const int h = kFocusRowH;
  if (focused) {
    c.fill_round_rect(x, y, w, h, h / 2, Gray::G0);
  } else {
    c.stroke_round_rect(x, y, w, h, h / 2, Gray::G0, 2, Gray::G3);
  }
  const Gray ink = focused ? Gray::G3 : Gray::G0;
  const Canvas::TextRole role = Canvas::TextRole::BodyStrong;
  const int lw = std::min(c.text_width(label, role), w - 48 - (has_lead ? 34 : 0));
  const int total = lw + (has_lead ? 34 : 0);
  int lx = x + (w - total) / 2;
  if (has_lead) {
    icon(c, lead, lx + 12, y + h / 2, 24, ink);
    lx += 34;
  }
  c.draw_text_fit(lx, y + (h - c.text_height(role)) / 2, lw, label, role, ink);
}

void empty_state(Canvas& c, Icon ic, std::string_view title, std::string_view body, int cy) {
  const int cx = kCanvasW / 2;
  const int ring_cy = cy - 70;
  c.stroke_circle(cx, ring_cy, 46, Gray::G0, 2);
  icon(c, ic, cx, ring_cy, 44, Gray::G0);
  const int tw = c.text_width(title, Canvas::TextRole::BodyStrong);
  c.draw_text(cx - tw / 2, cy, title, Canvas::TextRole::BodyStrong, Gray::G0);
  int y = cy + c.text_height(Canvas::TextRole::BodyStrong) + 6;
  for (const auto& line : wrap(c, body, Canvas::TextRole::Secondary, kContentW - 40)) {
    const int lw = c.text_width(line, Canvas::TextRole::Secondary);
    c.draw_text(cx - lw / 2, y, line, Canvas::TextRole::Secondary, Gray::G0);
    y += c.text_height(Canvas::TextRole::Secondary) + 2;
  }
}

void steps(Canvas& c, int step, int total) {
  if (total <= 0) return;
  char buf[24];
  std::snprintf(buf, sizeof(buf), "STEP %d OF %d", step, total);
  draw_tracked(c, kSideMargin, kContentTop - 4, buf, Canvas::TextRole::Caption, Gray::G0, 2);
  const int y = kContentTop + 26;
  const int gap = 6;
  const int seg = (kContentW - (total - 1) * gap) / total;
  for (int i = 0; i < total; ++i) {
    const int sx = kSideMargin + i * (seg + gap);
    if (i < step) c.fill_round_rect(sx, y, seg, 5, 2, Gray::G0);
    else c.hline(sx, y + 2, seg, Gray::G0);
  }
}

void footer(Canvas& c, std::string_view text) {
  c.fill_rect(0, kFooterY - 14, kCanvasW, kCanvasH - (kFooterY - 14), Gray::G3);
  c.dotted_hline(kSideMargin, kFooterY - 10, kContentW, Gray::G0, 3);
  c.draw_text_fit(kSideMargin, kFooterY - 2, kContentW, text, Canvas::TextRole::Caption, Gray::G0);
}

void toast(Canvas& c, std::string_view msg) {
  const auto lines = wrap(c, msg, Canvas::TextRole::Secondary, kContentW - 32);
  const int lh = c.text_height(Canvas::TextRole::Secondary);
  const int h = static_cast<int>(lines.size()) * lh + 18;
  const int y = kCanvasH - 16 - h;
  c.fill_rect(0, y - 6, kCanvasW, h + 22, Gray::G3);
  c.fill_round_rect(kSideMargin, y, kContentW, h, 14, Gray::G0);
  int ty = y + 9;
  for (const auto& line : lines) {
    c.draw_text(kSideMargin + 16, ty, line, Canvas::TextRole::Secondary, Gray::G3);
    ty += lh;
  }
}

int window_start(int focus, int count, int visible) {
  if (visible <= 0 || count <= visible) return 0;
  int start = focus - (visible - 2);
  if (start < 0) start = 0;
  if (start > count - visible) start = count - visible;
  return start;
}

void scrollbar(Canvas& c, int top, int height, int start, int visible, int count) {
  if (count <= visible || height <= 0) return;
  const int x = kCanvasW - 7;
  c.fill_rect(x, top, 3, height, Gray::G3);
  for (int y = top; y < top + height; y += 4) c.set_pixel(x + 1, y, Gray::G0);  // dotted track
  const int thumb_h = std::max(24, height * visible / count);
  const int thumb_y = top + (height - thumb_h) * start / std::max(1, count - visible);
  c.fill_round_rect(x, thumb_y, 3, thumb_h, 1, Gray::G0);
}

int kv(Canvas& c, int y, std::string_view key, std::string_view value) {
  c.draw_text(kSideMargin, y, key, Canvas::TextRole::Secondary, Gray::G0);
  const int vw = std::min(c.text_width(value, Canvas::TextRole::BodyStrong), kContentW * 3 / 5);
  c.draw_text_fit(kCanvasW - kSideMargin - vw, y - 2, vw, value, Canvas::TextRole::BodyStrong, Gray::G0);
  c.dotted_hline(kSideMargin, y + 36, kContentW, Gray::G0, 3);
  return y + 48;
}

std::vector<std::string> wrap_text(Canvas& c, std::string_view text, Canvas::TextRole role, int max_w) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= text.size()) {
    size_t nl = text.find('\n', p);
    if (nl == std::string_view::npos) nl = text.size();
    const std::string_view para = text.substr(p, nl - p);
    if (para.find_first_not_of(" \t\r") == std::string_view::npos) {
      out.emplace_back();
    } else {
      for (auto& l : wrap(c, para, role, max_w)) out.push_back(std::move(l));
    }
    if (nl == text.size()) break;
    p = nl + 1;
  }
  while (!out.empty() && out.back().empty()) out.pop_back();
  return out;
}

void waves(Canvas& c, int top, int h) {
  for (int j = 0; j < 9; ++j) {
    const float t = j / 8.0f;
    const int y0 = top + static_cast<int>(h * (0.15f + 0.8f * t));
    const float amp = 6.0f + 16.0f * t;
    const float freq = 0.028f - 0.012f * t;
    const int thick = 1 + static_cast<int>(3 * t);
    int prev = y0;
    for (int x = 0; x < kCanvasW; ++x) {
      const float sw = std::sin(x * freq + j * 1.7f);
      const int y = y0 - static_cast<int>(amp * (sw > 0 ? sw * sw : sw * 0.4f));
      const int a = x == 0 ? y : std::min(prev, y);
      const int b = x == 0 ? y : std::max(prev, y);
      c.fill_rect(x, a, 1, b - a + thick, Gray::G0);
      prev = y;
    }
  }
}

}  // namespace ui
}  // namespace pocket
