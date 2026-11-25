#include "pocket/app.hpp"
#include "pocket/ui.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pocket {
namespace {

static const char* kAppLabels[kHomeGridSlots] = {
    "Notes", "Ledger", "Clock", "Pass", "Weather", "Music", "Settings", "Update",
    "Reading", "Today", "",     "",     "",        "",      "",         "",
};

/** Home tile icon for each app (vector set in ui.cpp — one stroke weight throughout). */
ui::Icon app_icon(HomeApp app) {
  switch (app) {
    case HomeApp::Notes:
      return ui::Icon::Notes;
    case HomeApp::Ledger:
      return ui::Icon::Ledger;
    case HomeApp::Clock:
      return ui::Icon::Clock;
    case HomeApp::Pass:
      return ui::Icon::Pass;
    case HomeApp::Weather:
      return ui::Icon::Weather;
    case HomeApp::Music:
      return ui::Icon::Music;
    case HomeApp::Settings:
      return ui::Icon::Settings;
    case HomeApp::Update:
      return ui::Icon::Update;
    case HomeApp::Today:
      return ui::Icon::Calendar;
    case HomeApp::Reading:
    default:
      return ui::Icon::Reading;
  }
}

void draw_app_glyph(Canvas& c, HomeApp app, int cx, int cy, int size, Gray g) {
  ui::icon(c, app_icon(app), cx, cy, size * 52 / 100, g);
}

/** Display order: real content apps, then Reading, then the always-on Settings/Update tiles —
 * keeps the utility tiles last regardless of HomeApp's underlying (bitmask-compatible) index. */
constexpr int kHomeOrder[kHomeGridSlots] = {9, 0, 2, 4, 5, 8, 3, 6, 7, 1, 10, 11, 12, 13, 14, 15};

/** Effective Home order: the Companion-chosen order first, then anything it left out. */
std::vector<HomeApp> home_order(const DeviceConfig& cfg) {
  std::vector<HomeApp> out;
  auto add = [&](int v) {
    if (v < 0 || v >= kHomeGridSlots) return;
    const HomeApp a = static_cast<HomeApp>(v);
    if (std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
  };
  for (uint8_t v : cfg.home_order) add(v);
  for (int v : kHomeOrder) add(v);
  return out;
}

/** Home: editorial header (greeting + date) over an evenly spaced 3-column grid
 * that uses the whole page — no dead band at the bottom. */
constexpr int kHomeCols = 3;
constexpr int kHomeGapX = 18;
/** Header: greeting (ScreenTitle) + date (Secondary) + rule. */
constexpr int kHomeHeaderH = 112;
constexpr int kHomeBottomPad = 18;
constexpr int kHomeIconMax = 108;
constexpr int kHomeIconMin = 68;

/** Cell metrics: square icon + label band; rows share the remaining height evenly. */
void home_grid_metrics(int n_focus, int label_band_h, int* out_grid_top, int* out_cell_w,
                       int* out_cell_h, int* out_icon) {
  const int grid_top = kContentTop + kHomeHeaderH;
  const int rows = std::max(1, (n_focus + kHomeCols - 1) / kHomeCols);
  const int cell_w = (kCanvasW - 2 * kSideMargin - (kHomeCols - 1) * kHomeGapX) / kHomeCols;
  const int avail = kCanvasH - grid_top - kHomeBottomPad;
  const int row_h = std::min(avail / std::max(3, rows), kHomeIconMax + label_band_h + 60);
  int icon = std::min({cell_w - 16, kHomeIconMax, row_h - label_band_h - 18});
  icon = std::max(kHomeIconMin, icon);
  *out_grid_top = grid_top;
  *out_cell_w = cell_w;
  *out_cell_h = row_h;
  *out_icon = icon;
}

/** Word-wrap `text` into at most `max_lines` centered lines within `max_w`,
 * ellipsizing the final line if it still overflows. Draws nothing if empty. */
void draw_label_wrapped_centered(Canvas& c, int cx, int top_y, int max_w, int max_lines,
                                 const char* text, Gray g) {
  if (!text || !*text) return;
  const Canvas::TextRole role = Canvas::TextRole::Secondary;
  const int line_h = c.text_height(role) + 4;
  std::string s(text);

  // Fits on one line — common case for current labels.
  if (c.text_width(s, role) <= max_w) {
    c.draw_text_centered(cx, top_y, s, role, g);
    return;
  }

  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < s.size() && static_cast<int>(lines.size()) < max_lines) {
    size_t best = pos;
    size_t scan = pos;
    while (scan <= s.size()) {
      size_t next_space = s.find(' ', scan);
      size_t end = (next_space == std::string::npos) ? s.size() : next_space;
      if (c.text_width(s.substr(pos, end - pos), role) <= max_w) {
        best = end;
        if (end >= s.size()) break;
        scan = end + 1;
      } else {
        break;
      }
    }
    if (best == pos) {
      // Single token wider than max_w — hard cut.
      size_t cut = pos + 1;
      while (cut < s.size() && c.text_width(s.substr(pos, cut - pos), role) <= max_w) ++cut;
      best = std::max(pos + 1, cut - 1);
    }
    lines.push_back(s.substr(pos, best - pos));
    pos = best;
    while (pos < s.size() && s[pos] == ' ') ++pos;
  }
  if (pos < s.size() && !lines.empty()) {
    std::string& last = lines.back();
    std::string ell = "...";
    while (!last.empty() && c.text_width(last + ell, role) > max_w) last.pop_back();
    last += ell;
  }
  int y = top_y - (static_cast<int>(lines.size()) - 1) * line_h;
  for (const auto& line : lines) {
    c.draw_text_centered(cx, y, line, role, g);
    y += line_h;
  }
}

int count_home_focusable(const DeviceConfig& cfg, bool has_passes, HomeApp* out, int cap, int n_ext) {
  int n = 0;
  for (const HomeApp a : home_order(cfg)) {
    if (n >= cap) break;
    // Apps from Northwork take the free tiles Slot10.. in install order.
    const int slot = static_cast<int>(a) - static_cast<int>(HomeApp::Slot10);
    if (slot >= 0 && slot < kExtAppMax) {
      if (slot < n_ext) out[n++] = a;
      continue;
    }
    if (!home_app_visible(cfg, a)) continue;
    // Never show placeholders on Home: Ledger isn't built yet, and Pass only has
    // something to open once passes exist on the device.
    if (a == HomeApp::Ledger) continue;
    if (a == HomeApp::Pass && !has_passes) continue;
    out[n++] = a;
  }
  return n;
}

}  // namespace

void App::render_home() {
  draw_status_bar();

  // Header — a calm greeting and today's date instead of a product label.
  static const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                                "Saturday"};
  static const char* kMonthsLong[] = {"January", "February", "March",     "April",   "May",      "June",
                                      "July",    "August",   "September", "October", "November", "December"};
  int hh = 0, mm = 0, wd = 0, mo = 0, day = 0;
  clock_.local_hm(hh, mm, wd, mo, day);
  const char* greet = "Good evening";
  if (!clock_.time_valid()) greet = "Hello";
  else if (hh >= 4 && hh < 12) greet = "Good morning";
  else if (hh >= 12 && hh < 17) greet = "Good afternoon";
  canvas_.draw_text(kSideMargin, kContentTop, greet, Canvas::TextRole::ScreenTitle, Gray::G0);
  if (clock_.time_valid()) {
    char date[48];
    std::snprintf(date, sizeof(date), "%s, %s %d", kDays[((wd % 7) + 7) % 7], kMonthsLong[((mo % 12) + 12) % 12],
                  day);
    canvas_.draw_text(kSideMargin, kContentTop + 50, date, Canvas::TextRole::Secondary, Gray::G0);
  }
  canvas_.dotted_hline(kSideMargin, kContentTop + kHomeHeaderH - 22, kContentW, Gray::G0, 3);

  HomeApp focusable[kHomeGridSlots];
  const int n_focus = count_home_focusable(cfg_, !data_.passes.empty(), focusable, kHomeGridSlots, static_cast<int>(ext_apps_.size()));
  focus_.count = std::max(1, n_focus);
  if (focus_.index >= focus_.count) focus_.index = 0;
  const HomeApp focused = n_focus > 0 ? focusable[focus_.index] : HomeApp::Settings;

  const int label_band_h = canvas_.text_height(Canvas::TextRole::Secondary) + 6;
  int grid_top = 0, cell_w = 0, cell_h = 0, icon = 0;
  home_grid_metrics(n_focus, label_band_h, &grid_top, &cell_w, &cell_h, &icon);
  const int radius = std::max(14, icon * 26 / 100);

  for (int i = 0; i < n_focus; ++i) {
    const int col = i % kHomeCols;
    const int row = i / kHomeCols;
    const int cell_x = kSideMargin + col * (cell_w + kHomeGapX);
    const int cell_y = grid_top + row * cell_h;
    const HomeApp a = focusable[i];
    const bool is_focus = a == focused;
    const int label_i = static_cast<int>(a);
    const int ext_i = label_i - static_cast<int>(HomeApp::Slot10);
    const ExtApp* ext = ext_i >= 0 && ext_i < static_cast<int>(ext_apps_.size()) ? &ext_apps_[static_cast<size_t>(ext_i)] : nullptr;
    const char* label = ext ? ext->name.c_str() : (label_i >= 0 && label_i < kHomeGridSlots) ? kAppLabels[label_i] : "";

    const int block_h = icon + label_band_h;
    const int icon_x = cell_x + (cell_w - icon) / 2;
    const int icon_y = cell_y + std::max(0, (cell_h - block_h) / 2 - 4);
    const int label_top = icon_y + icon + 6;

    if (is_focus) {
      canvas_.fill_round_rect(icon_x, icon_y, icon, icon, radius, Gray::G0);
      if (ext) ext_app_icon(canvas_, *ext, icon_x + icon / 2, icon_y + icon / 2, icon * 52 / 100, Gray::G3);
      else draw_app_glyph(canvas_, a, icon_x + icon / 2, icon_y + icon / 2, icon, Gray::G3);
    } else {
      canvas_.stroke_round_rect(icon_x, icon_y, icon, icon, radius, Gray::G0, 2, Gray::G3);
      if (ext) ext_app_icon(canvas_, *ext, icon_x + icon / 2, icon_y + icon / 2, icon * 52 / 100, Gray::G0);
      else draw_app_glyph(canvas_, a, icon_x + icon / 2, icon_y + icon / 2, icon, Gray::G0);
    }
    const Canvas::TextRole role = is_focus ? Canvas::TextRole::BodyStrong : Canvas::TextRole::Secondary;
    const int lw = canvas_.text_width(label, role);
    const int ly = label_top + (is_focus ? -2 : 0);
    if (lw <= cell_w) {
      canvas_.draw_text(cell_x + (cell_w - lw) / 2, ly, label, role, Gray::G0);
    } else {
      draw_label_wrapped_centered(canvas_, cell_x + cell_w / 2, ly, cell_w - 4, 1, label, Gray::G0);
    }
  }
  if (now_ms_ < error_until_ms_ && !error_msg_.empty()) ui::toast(canvas_, error_msg_);
}

void App::mark_home_focus_dirty(int prev_index, int next_index) {
  ++home_focus_spin_count_;
  (void)prev_index;
  (void)next_index;
  // Always content-band for Home focus moves. Tight tile unions were leaving stale
  // icon fragments on the e-ink panel (duplicate glyphs / rearranged grid) because
  // rounded plates + 1-bit glyph blits need a full content wipe under fast 0xFF.
  // Still a partial LUT — not a slow full refresh.
  mark_content_dirty();
}

bool App::is_home_screen() const { return nav_.current() == ScreenId::Home; }

void App::apply_home_focus_delta(int delta) {
  if (!is_home_screen() || delta == 0) return;
  HomeApp focusable[kHomeGridSlots];
  const int n = count_home_focusable(cfg_, !data_.passes.empty(), focusable, kHomeGridSlots, static_cast<int>(ext_apps_.size()));
  focus_.count = std::max(1, n);
  if (focus_.index >= focus_.count) focus_.index = 0;
  const int prev = focus_.index;
  focus_.move(delta);
  if (focus_.index == prev) return;
  mark_home_focus_dirty(prev, focus_.index);
}

void App::handle_home(InputEvent e) {
  HomeApp focusable[kHomeGridSlots];
  const int n = count_home_focusable(cfg_, !data_.passes.empty(), focusable, kHomeGridSlots, static_cast<int>(ext_apps_.size()));
  focus_.count = std::max(1, n);
  if (e == InputEvent::Up || e == InputEvent::Down) {
    apply_home_focus_delta(e == InputEvent::Up ? -1 : 1);
  } else if (e == InputEvent::Select && n > 0) {
    const HomeApp launch = focusable[focus_.index];
    if (parental_requires_pin(launch) && !parental_session_unlocked_) {
      parental_pending_app_ = launch;
      parental_pin_for_app_ = true;
      pin_entry_.clear();
      pin_spin_count_ = 0;
      pin_ignore_select_until_ms_ = 0;
      pin_enter_empty_slot();
      nav_.push(ScreenId::Pin);
      after_nav();
      return;
    }
    launch_home_app(launch);
  } else if (e == InputEvent::Back) {
    // stay on home
  }
}

}  // namespace pocket
