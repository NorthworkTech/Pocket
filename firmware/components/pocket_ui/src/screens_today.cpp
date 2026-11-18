/** Today — a calm daily briefing: weather, alarms/timer, what's left on your lists, latest note.
 * Each block is a row; pressing it opens that app. */
#include "pocket/app.hpp"
#include "pocket/ui.hpp"
#include <cstdio>
#include <ctime>
#include <string>

namespace pocket {

namespace {
constexpr int kBlocks = 4;  // Weather, Clock, Lists, Notes
constexpr int kBlockPitch = kRowPitch + 64;

std::string hm_label(int minutes, int fmt) {
  char b[16];
  const int h = minutes / 60, m = minutes % 60;
  if (fmt == 24) std::snprintf(b, sizeof(b), "%02d:%02d", h, m);
  else std::snprintf(b, sizeof(b), "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
  return b;
}
}  // namespace

void App::render_today() {
  draw_status_bar();
  ui::header(canvas_, "Today", true);
  using ui::Icon;
  using ui::Row;
  using ui::RowKind;

  // Weather.
  std::string w_line = "No forecast yet", w_meta = "Open Weather to set your city";
  if (!data_.weather.days.empty()) {
    const auto& d0 = data_.weather.days[0];
    char b[96];
    std::snprintf(b, sizeof(b), "%d\xC2\xB0 \xC2\xB7 %s", weather_display_temp(data_.weather.today_temp),
                  data_.weather.today_condition.c_str());
    w_line = b;
    std::snprintf(b, sizeof(b), "High %d\xC2\xB0, low %d\xC2\xB0", weather_display_temp(d0.hi), weather_display_temp(d0.lo));
    w_meta = b;
    const std::string upd = weather_updated_label();
    if (!upd.empty()) w_meta += " \xC2\xB7 " + upd;
  }

  // Clock: running timer wins, else the next alarm.
  std::string c_line = "No alarms set", c_meta = "Open Clock to add one";
  if (timer_running_ && !timer_paused_ && timer_end_ms_ > now_ms_) {
    const uint32_t left = (timer_end_ms_ - now_ms_ + 999) / 1000;
    char b[48];
    std::snprintf(b, sizeof(b), "Timer \xC2\xB7 %u:%02u left", static_cast<unsigned>(left / 60),
                  static_cast<unsigned>(left % 60));
    c_line = b;
    c_meta = "Running";
  } else if (clock_.time_valid()) {
    int h = 0, m = 0, wd = 0, mo = 0, dd = 0;
    clock_.local_hm(h, m, wd, mo, dd);
    const int now = h * 60 + m;
    const Alarm* best = nullptr;
    int best_key = 1 << 30;
    int enabled = 0;
    for (const auto& a : data_.alarms) {
      if (!a.enabled) continue;
      ++enabled;
      const int key = a.minutes_of_day > now ? a.minutes_of_day - now : a.minutes_of_day + 1440 - now;
      if (key < best_key) {
        best_key = key;
        best = &a;
      }
    }
    if (best) {
      c_line = "Next alarm " + hm_label(best->minutes_of_day, cfg_.time_format);
      c_meta = (best->minutes_of_day > now ? "Today" : "Tomorrow");
      if (!best->label.empty()) c_meta += " \xC2\xB7 " + best->label;
      if (enabled > 1) c_meta += " \xC2\xB7 " + std::to_string(enabled) + " alarms on";
    }
  }

  // Lists: first list with something left to do.
  std::string l_line = "Nothing on your lists", l_meta = "Add lists in Notes or Companion";
  for (const auto& L : data_.lists) {
    int open = 0;
    std::string first;
    for (const auto& it : L.items)
      if (!it.checked) {
        if (open == 0) first = it.text;
        ++open;
      }
    if (open == 0) continue;
    l_line = L.title + " \xC2\xB7 " + std::to_string(open) + (open == 1 ? " to do" : " to do");
    l_meta = "Next: " + first;
    break;
  }

  // Latest note.
  std::string n_line = "No notes yet", n_meta = "Hold the back button in Notes to dictate";
  if (!data_.notes.empty()) {
    const Note* latest = &data_.notes.front();
    for (const auto& n : data_.notes)
      if (n.updated_at > latest->updated_at) latest = &n;
    n_line = latest->title.empty() ? std::string("Untitled note") : latest->title;
    n_meta = latest->pending_pcm.empty() ? std::string("Latest note") : std::string("Voice note \xC2\xB7 waiting for Wi-Fi");
  }

  static const char* kSections[kBlocks] = {"WEATHER", "CLOCK", "LISTS", "NOTES"};
  static const Icon kIcons[kBlocks] = {Icon::Weather, Icon::Alarm, Icon::Check, Icon::Notes};
  const std::string* lines[kBlocks] = {&w_line, &c_line, &l_line, &n_line};
  const std::string* metas[kBlocks] = {&w_meta, &c_meta, &l_meta, &n_meta};
  focus_.count = kBlocks;
  for (int i = 0; i < kBlocks; ++i) {
    const int y = kListTop + i * kBlockPitch;
    ui::section(canvas_, y, kSections[i]);
    ui::row(canvas_, y + 30, Row{*lines[i], RowKind::Nav, {}, false, kIcons[i], true}, focus_.index == i);
    canvas_.draw_text_fit(kSideMargin + 12, y + 30 + kFocusRowH + 2, kContentW - 12, *metas[i],
                          Canvas::TextRole::Caption, Gray::G0);
  }
}

void App::handle_today(InputEvent e) {
  focus_.count = kBlocks;
  if (e == InputEvent::Up || e == InputEvent::Down) {
    const int prev = focus_.index;
    focus_.move(e == InputEvent::Down ? 1 : -1);
    if (focus_.index != prev) mark_content_dirty();
    return;
  }
  if (e == InputEvent::Back) {
    if (!nav_.pop()) nav_.reset(ScreenId::Home);
    after_nav();
    return;
  }
  if (e != InputEvent::Select) return;
  static const HomeApp kDest[kBlocks] = {HomeApp::Weather, HomeApp::Clock, HomeApp::Notes, HomeApp::Notes};
  if (focus_.index == 2) {
    // Lists live on the Notes app's Lists tab.
    nav_.pop();
    launch_home_app(HomeApp::Notes);
    notes_tab_ = 1;
    nav_.replace(ScreenId::ListsList);
    after_nav();
    return;
  }
  nav_.pop();
  launch_home_app(kDest[focus_.index]);
}

}  // namespace pocket
