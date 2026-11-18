/**
 * Clock app — face, alarms, timers, and the alert screen.
 *
 * Alarms persist to <data_root>/alarms.txt (written atomically). They fire from
 * tick() on the minute, even from the lock screen; the alert beeps until Dismiss
 * / Snooze (9 min) or five minutes pass. One countdown timer runs in the
 * background; its screen repaints only the digits once a second.
 */
#include "pocket/app.hpp"
#include "pocket/ui.hpp"
#include "pocket/json_lite.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace pocket {

namespace {

constexpr int kSnoozeMinutes = 9;
constexpr uint32_t kRingMaxMs = 5u * 60u * 1000u;
constexpr uint32_t kBeepEveryMs = 2500;
constexpr int kTimerPresetsMin[] = {1, 3, 5, 10, 15, 25, 45, 60};
constexpr int kTimerPresetCount = sizeof(kTimerPresetsMin) / sizeof(kTimerPresetsMin[0]);

const char* kWeekdayNames[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* kMonthNames[] = {"January", "February", "March",     "April",   "May",      "June",
                             "July",    "August",   "September", "October", "November", "December"};

/** "7:05" + "AM" (12h) or "07:05" + "" (24h). */
void format_hm(int minutes_of_day, int time_format, char* hm, size_t n, const char** suffix) {
  const int h = (minutes_of_day / 60) % 24;
  const int m = minutes_of_day % 60;
  if (time_format == 24) {
    std::snprintf(hm, n, "%02d:%02d", h, m);
    *suffix = "";
  } else {
    int h12 = h % 12;
    if (h12 == 0) h12 = 12;
    std::snprintf(hm, n, "%d:%02d", h12, m);
    *suffix = h < 12 ? "AM" : "PM";
  }
}

std::string alarm_label(const Alarm& a, int time_format) {
  char hm[16];
  const char* suf = "";
  format_hm(a.minutes_of_day, time_format, hm, sizeof(hm), &suf);
  std::string s = hm;
  if (*suf) {
    s += ' ';
    s += suf;
  }
  return s;
}


/** Big centered time with a small AM/PM beside it. */
void draw_big_time(Canvas& c, int y, const char* hm, const char* suffix, Canvas::TextRole role) {
  const int tw = c.text_width(hm, role);
  const int sw = *suffix ? c.text_width(suffix, Canvas::TextRole::BodyStrong) + 8 : 0;
  const int x = (kCanvasW - tw - sw) / 2;
  c.draw_text(x, y, hm, role, Gray::G0);
  if (*suffix) {
    c.draw_text(x + tw + 8, y + c.text_height(role) - c.text_height(Canvas::TextRole::BodyStrong) - 18, suffix,
                Canvas::TextRole::BodyStrong, Gray::G0);
  }
}

/** Geometry of the countdown digits on ClockTimerRun (repainted alone each second). */
constexpr int kTimerDigitsY = kListTop + 40;

}  // namespace

// ── Persistence ─────────────────────────────────────────────────────────

void App::load_alarms() {
  data_.alarms.clear();
  if (!storage_ || !storage_->data_ensure_root()) return;
  std::ifstream in(storage_->data_root() + "/alarms.txt");
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    Alarm a;
    int enabled = 1;
    if (!(ls >> a.id >> a.minutes_of_day >> enabled)) continue;
    if (a.minutes_of_day < 0 || a.minutes_of_day >= 24 * 60) continue;
    a.enabled = enabled != 0;
    data_.alarms.push_back(a);
    if (data_.alarms.size() >= 12) break;
  }
}

void App::save_alarms() {
  std::sort(data_.alarms.begin(), data_.alarms.end(),
            [](const Alarm& a, const Alarm& b) { return a.minutes_of_day < b.minutes_of_day; });
  if (!storage_ || !storage_->data_ensure_root()) return;
  std::string out;
  for (const auto& a : data_.alarms) {
    out += a.id + " " + std::to_string(a.minutes_of_day) + " " + (a.enabled ? "1" : "0") + "\n";
  }
  if (!write_file_atomic(storage_->data_root() + "/alarms.txt", out)) {
    error_msg_ = "Couldn't save alarms.";
    error_until_ms_ = now_ms_ + 3000;
  }
}

// ── Background events ───────────────────────────────────────────────────

uint32_t App::timer_remaining_ms() const {
  if (!timer_running_) return 0;
  if (timer_paused_) return timer_paused_left_ms_;
  return now_ms_ >= timer_end_ms_ ? 0 : timer_end_ms_ - now_ms_;
}

void App::start_ringing(bool from_timer) {
  // An alert must be heard: stop music (tones are skipped while a track owns the speaker).
  if (audio_ && music_playing_) audio_->stop();
  music_playing_ = false;
  ring_from_timer_ = from_timer;
  ring_next_beep_ms_ = now_ms_;
  ring_until_ms_ = now_ms_ + kRingMaxMs;
  focus_.index = 0;
  if (nav_.current() != ScreenId::ClockRinging) nav_.push(ScreenId::ClockRinging);
  after_nav();
}

void App::stop_ringing(bool snooze) {
  if (snooze && !ring_from_timer_) {
    int h = 0, m = 0, wd = 0, mo = 0, d = 0;
    clock_.local_hm(h, m, wd, mo, d);
    snooze_minutes_ = (h * 60 + m + kSnoozeMinutes) % (24 * 60);
  }
  if (ring_from_timer_) timer_running_ = false;
  ring_until_ms_ = 0;
  if (nav_.current() == ScreenId::ClockRinging) {
    if (!nav_.pop()) nav_.reset(cfg_.onboarding_complete ? ScreenId::Home : ScreenId::OnboardingWelcome);
    // Never drop back into a stale timer screen.
    if (nav_.current() == ScreenId::ClockTimerRun && !timer_running_) nav_.replace(ScreenId::ClockTimers);
    focus_.index = 0;
    after_nav(true);
  }
}

void App::maybe_tick_clock_events() {
  if (!cfg_.onboarding_complete) return;

  // Timer completion.
  if (timer_running_ && !timer_paused_ && now_ms_ >= timer_end_ms_ && nav_.current() != ScreenId::ClockRinging) {
    timer_end_ms_ = now_ms_;
    start_ringing(/*from_timer=*/true);
    return;
  }

  // Alarms: evaluate once per wall-clock minute.
  if (clock_.time_valid()) {
    int h = 0, m = 0, wd = 0, mo = 0, d = 0;
    clock_.local_hm(h, m, wd, mo, d);
    const int minute = h * 60 + m;
    const int key = ((mo * 31 + d) * 1440) + minute;
    if (key != last_alarm_minute_key_) {
      const bool first = last_alarm_minute_key_ < 0;
      last_alarm_minute_key_ = key;
      bool due = false;
      if (!first) {
        for (const auto& a : data_.alarms) due = due || (a.enabled && a.minutes_of_day == minute);
        if (snooze_minutes_ == minute) {
          due = true;
          snooze_minutes_ = -1;
        }
      }
      if (due && nav_.current() != ScreenId::ClockRinging) {
        start_ringing(/*from_timer=*/false);
        return;
      }
    }
  }

  const ScreenId s = nav_.current();
  if (s == ScreenId::ClockRinging) {
    if (now_ms_ >= ring_until_ms_) {
      stop_ringing(false);
      return;
    }
    if (now_ms_ >= ring_next_beep_ms_) {
      ring_next_beep_ms_ = now_ms_ + kBeepEveryMs;
      play_sound_urgent(SoundId::Attention);  // alarms ring even in quiet hours
    }
    return;
  }

  // Live countdown digits.
  if (s == ScreenId::ClockTimerRun && timer_running_) {
    const int secs = static_cast<int>((timer_remaining_ms() + 999) / 1000);
    if (secs != timer_shown_s_) {
      timer_shown_s_ = secs;
      mark_region_dirty(0, kTimerDigitsY - 8, kCanvasW, canvas_.text_height(Canvas::TextRole::HugeClock) + 60);
      if (dirty_ && try_present(false)) dirty_ = false;
    }
  }
}

void App::open_alarm_editor(int index) {
  alarm_edit_index_ = index;
  alarm_adjust_ = 0;
  if (index >= 0 && index < static_cast<int>(data_.alarms.size())) {
    alarm_draft_ = data_.alarms[static_cast<size_t>(index)];
  } else {
    alarm_draft_ = Alarm{};
    alarm_draft_.id = "a" + std::to_string(now_ms_ % 100000) + std::to_string(data_.alarms.size());
    alarm_draft_.minutes_of_day = 7 * 60;
    alarm_draft_.enabled = true;
  }
  focus_.index = 0;
  nav_.push(ScreenId::ClockAlarmEdit);
  after_nav();
}

// ── Render ──────────────────────────────────────────────────────────────

void App::render_clock() {
  using ui::Icon;
  using ui::Row;
  using ui::RowKind;
  draw_status_bar();
  const ScreenId s = nav_.current();
  int h = 0, m = 0, wd = 0, mo = 0, d = 0;
  clock_.local_hm(h, m, wd, mo, d);

  if (s == ScreenId::ClockRinging) {
    char hm[16];
    const char* suf = "";
    format_hm(h * 60 + m, cfg_.time_format, hm, sizeof(hm), &suf);
    canvas_.stroke_circle(kCanvasW / 2, 176, 54, Gray::G0, 3);
    ui::icon(canvas_, ring_from_timer_ ? Icon::Timer : Icon::Alarm, kCanvasW / 2, 176, 60, Gray::G0);
    const char* title = ring_from_timer_ ? "Time's up" : "Alarm";
    const int tw = canvas_.text_width(title, Canvas::TextRole::ScreenTitle);
    canvas_.draw_text((kCanvasW - tw) / 2, 250, title, Canvas::TextRole::ScreenTitle, Gray::G0);
    draw_big_time(canvas_, 310, hm, suf, Canvas::TextRole::HugeClock);
    focus_.count = ring_from_timer_ ? 1 : 2;
    ui::button(canvas_, ring_from_timer_ ? kBottomCtaY : kBottomCtaY - kRowPitch, "Dismiss", focus_.index == 0);
    if (!ring_from_timer_) {
      char snooze[24];
      std::snprintf(snooze, sizeof(snooze), "Snooze %d min", kSnoozeMinutes);
      ui::row(canvas_, kBottomCtaY, Row{snooze, RowKind::Plain, {}, false, Icon::Alarm, true}, focus_.index == 1);
    }
    return;
  }

  if (s == ScreenId::ClockAlarms) {
    ui::header(canvas_, "Alarms", true);
    const int n = static_cast<int>(data_.alarms.size());
    focus_.count = n + 2;
    if (focus_.index >= focus_.count) focus_.index = focus_.count - 1;
    int y = kListTop;
    if (n == 0) {
      ui::empty_state(canvas_, Icon::Alarm, "No alarms yet", "Pocket rings even when it's locked.", 330);
      y = kBottomCtaY - kRowPitch;
    }
    for (int i = 0; i < n && i < 8; ++i) {
      const auto& a = data_.alarms[static_cast<size_t>(i)];
      const std::string label = alarm_label(a, cfg_.time_format);
      ui::row(canvas_, y, Row{label, RowKind::Toggle, {}, a.enabled, Icon::Alarm, true}, focus_.index == i);
      y += kRowPitch;
    }
    ui::row(canvas_, y, Row{"New alarm", RowKind::Nav, {}, false, Icon::Plus, true}, focus_.index == n);
    ui::row(canvas_, y + kRowPitch, Row{"Back", RowKind::Back}, focus_.index == n + 1);
    if (now_ms_ < error_until_ms_ && !error_msg_.empty()) ui::toast(canvas_, error_msg_);
    return;
  }

  if (s == ScreenId::ClockAlarmEdit) {
    ui::header(canvas_, alarm_edit_index_ < 0 ? "New alarm" : "Edit alarm", true);
    char hm[16];
    const char* suf = "";
    format_hm(alarm_draft_.minutes_of_day, cfg_.time_format, hm, sizeof(hm), &suf);
    draw_big_time(canvas_, kListTop - 4, hm, suf, Canvas::TextRole::Hero);
    const int top = kListTop + 120;
    const bool existing = alarm_edit_index_ >= 0;
    focus_.count = existing ? 5 : 4;
    char hv[16], mv[16];
    const int hh = alarm_draft_.minutes_of_day / 60;
    const int mm = alarm_draft_.minutes_of_day % 60;
    if (cfg_.time_format == 24) std::snprintf(hv, sizeof(hv), "%02d", hh);
    else std::snprintf(hv, sizeof(hv), "%d %s", hh % 12 == 0 ? 12 : hh % 12, hh < 12 ? "AM" : "PM");
    std::snprintf(mv, sizeof(mv), "%02d", mm);
    std::string hlabel = alarm_adjust_ == 1 ? std::string("\xE2\x80\xB9  ") + hv + "  \xE2\x80\xBA" : std::string(hv);
    std::string mlabel = alarm_adjust_ == 2 ? std::string("\xE2\x80\xB9  ") + mv + "  \xE2\x80\xBA" : std::string(mv);
    int i = 0;
    ui::row(canvas_, row_y(top, i), Row{"Hour", RowKind::Value, hlabel}, focus_.index == i);
    ++i;
    ui::row(canvas_, row_y(top, i), Row{"Minute", RowKind::Value, mlabel}, focus_.index == i);
    ++i;
    ui::row(canvas_, row_y(top, i), Row{"Alarm on", RowKind::Toggle, {}, alarm_draft_.enabled}, focus_.index == i);
    ++i;
    if (existing) {
      ui::row(canvas_, row_y(top, i), Row{"Delete alarm", RowKind::Plain, {}, false, Icon::Trash, true},
              focus_.index == i);
      ++i;
    }
    ui::button(canvas_, kBottomCtaY, "Save", focus_.index == i, Icon::Check, true);
    if (alarm_adjust_ != 0) {
      canvas_.draw_text_centered(kCanvasW / 2, row_y(top, i) + 8, "Turn to change \xC2\xB7 Press when done",
                                 Canvas::TextRole::Caption, Gray::G0);
    }
    return;
  }

  if (s == ScreenId::ClockTimers) {
    ui::header(canvas_, "Timer", true);
    focus_.count = kTimerPresetCount + 1;
    for (int i = 0; i < kTimerPresetCount; ++i) {
      char label[24];
      std::snprintf(label, sizeof(label), "%d %s", kTimerPresetsMin[i], kTimerPresetsMin[i] == 1 ? "minute" : "minutes");
      ui::row(canvas_, row_y(kListTop, i), Row{label, RowKind::Nav, {}, false, Icon::Timer, true}, focus_.index == i);
    }
    ui::row(canvas_, row_y(kListTop, kTimerPresetCount), Row{"Back", RowKind::Back},
            focus_.index == kTimerPresetCount);
    return;
  }

  if (s == ScreenId::ClockTimerRun) {
    ui::header(canvas_, "Timer", true);
    const uint32_t left = timer_remaining_ms();
    const int secs = static_cast<int>((left + 999) / 1000);
    char buf[16];
    if (secs >= 3600) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
    else std::snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
    timer_shown_s_ = secs;
    canvas_.fill_rect(0, kTimerDigitsY - 8, kCanvasW, canvas_.text_height(Canvas::TextRole::HugeClock) + 60, Gray::G3);
    canvas_.draw_text_centered(kCanvasW / 2, kTimerDigitsY, buf,
                               secs >= 3600 ? Canvas::TextRole::Hero : Canvas::TextRole::HugeClock, Gray::G0);
    // Progress bar under the digits.
    const int by = kTimerDigitsY + canvas_.text_height(Canvas::TextRole::HugeClock) + 24;
    canvas_.stroke_round_rect(kSideMargin, by, kContentW, 14, 7, Gray::G0, 2, Gray::G3);
    if (timer_total_ms_ > 0) {
      const int fill = static_cast<int>(static_cast<uint64_t>(kContentW - 8) * (timer_total_ms_ - left) / timer_total_ms_);
      if (fill > 0) canvas_.fill_round_rect(kSideMargin + 4, by + 4, std::max(6, fill), 6, 3, Gray::G0);
    }
    char of[32];
    std::snprintf(of, sizeof(of), "%s of %u min", timer_paused_ ? "Paused" : "Running",
                  static_cast<unsigned>(timer_total_ms_ / 60000));
    canvas_.draw_text_centered(kCanvasW / 2, by + 30, of, Canvas::TextRole::Caption, Gray::G0);
    focus_.count = 2;
    ui::button(canvas_, kBottomCtaY - kRowPitch, timer_paused_ ? "Resume" : "Pause", focus_.index == 0,
               timer_paused_ ? Icon::Play : Icon::Pause, true);
    ui::row(canvas_, kBottomCtaY, Row{"Cancel timer", RowKind::Plain, {}, false, Icon::Stop, true},
            focus_.index == 1);
    return;
  }

  // ClockFace — tabs preview the three areas; Select opens Alarms / Timer.
  static const char* kClockTabs[] = {"Clock", "Alarms", "Timer"};
  ui::tabs(canvas_, kContentTop, kClockTabs, 3, clock_tab_);
  if (clock_tab_ == 0) {
    char tbuf[16];
    const char* suf = "";
    if (!clock_.time_valid()) {
      std::snprintf(tbuf, sizeof(tbuf), "--:--");
    } else {
      format_hm(h * 60 + m, cfg_.time_format, tbuf, sizeof(tbuf), &suf);
    }
    canvas_.draw_text_centered(kCanvasW / 2, kContentTop + kTabBand + 80, tbuf, Canvas::TextRole::HugeClock, Gray::G0);
    const int below = kContentTop + kTabBand + 80 + canvas_.text_height(Canvas::TextRole::HugeClock) + 16;
    if (clock_.time_valid() && wd >= 0 && wd < 7) {
      char datebuf[48];
      std::snprintf(datebuf, sizeof(datebuf), "%s%s%s, %s %d", suf, *suf ? " \xC2\xB7 " : "", kWeekdayNames[wd],
                    kMonthNames[((mo % 12) + 12) % 12], d);
      canvas_.draw_text_centered(kCanvasW / 2, below, datebuf, Canvas::TextRole::Secondary, Gray::G0);
    }
    // Next alarm line.
    int next = -1;
    int best = 1441;
    for (const auto& a : data_.alarms) {
      if (!a.enabled) continue;
      const int delta = (a.minutes_of_day - (h * 60 + m) + 1440) % 1440;
      if (delta < best) {
        best = delta;
        next = a.minutes_of_day;
      }
    }
    if (next >= 0) {
      Alarm tmp;
      tmp.minutes_of_day = next;
      const std::string line = "Next alarm " + alarm_label(tmp, cfg_.time_format);
      const int lw = canvas_.text_width(line, Canvas::TextRole::Caption) + 30;
      const int lx = (kCanvasW - lw) / 2;
      ui::icon(canvas_, Icon::Alarm, lx + 10, below + 56, 20, Gray::G0);
      canvas_.draw_text(lx + 30, below + 44, line, Canvas::TextRole::Caption, Gray::G0);
    }
    if (!clock_.time_valid() || clock_.time_needs_network_sync()) {
      ui::footer(canvas_, wifi_.connected() ? "Syncing time\xE2\x80\xA6"
                                            : "Connect to Wi-Fi once to set the time accurately.");
    }
  } else if (clock_tab_ == 1) {
    const int n = static_cast<int>(data_.alarms.size());
    if (n == 0) {
      ui::empty_state(canvas_, Icon::Alarm, "No alarms yet", "Press the dial to add one.", 360);
    } else {
      int y = kContentTop + kTabBand + 8;
      for (int i = 0; i < n && i < 7; ++i) {
        const auto& a = data_.alarms[static_cast<size_t>(i)];
        ui::row(canvas_, y, Row{alarm_label(a, cfg_.time_format), RowKind::Value, a.enabled ? "On" : "Off", false,
                                Icon::Alarm, true},
                false);
        y += kRowPitch;
      }
    }
    ui::button(canvas_, kBottomCtaY, "Open alarms", true, Icon::ChevronRight, true);
  } else {
    if (timer_running_) {
      const int secs = static_cast<int>((timer_remaining_ms() + 999) / 1000);
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
      canvas_.draw_text_centered(kCanvasW / 2, kContentTop + kTabBand + 100, buf, Canvas::TextRole::Hero, Gray::G0);
      canvas_.draw_text_centered(kCanvasW / 2, kContentTop + kTabBand + 210, timer_paused_ ? "Paused" : "Running",
                                 Canvas::TextRole::Caption, Gray::G0);
      ui::button(canvas_, kBottomCtaY, "Open timer", true, Icon::ChevronRight, true);
    } else {
      ui::empty_state(canvas_, Icon::Timer, "No timer running", "Press the dial to pick a length.", 360);
      ui::button(canvas_, kBottomCtaY, "Start a timer", true, Icon::Play, true);
    }
  }
}

// ── Input ───────────────────────────────────────────────────────────────

void App::handle_clock(InputEvent e) {
  const ScreenId s = nav_.current();
  const bool rot = e == InputEvent::Up || e == InputEvent::Down;
  const int dir = e == InputEvent::Down ? 1 : -1;

  if (s == ScreenId::ClockRinging) {
    if (rot) {
      const int prev = focus_.index;
      focus_.move(dir);
      if (focus_.index != prev) mark_content_dirty();
    } else if (e == InputEvent::Select) {
      stop_ringing(focus_.index == 1);
    } else if (e == InputEvent::Back) {
      stop_ringing(false);
    }
    return;
  }

  if (s == ScreenId::ClockFace) {
    if (e == InputEvent::Back) {
      nav_.pop();
      after_nav();
    } else if (rot) {
      clock_tab_ = (clock_tab_ + (dir > 0 ? 1 : 2)) % 3;
      mark_content_dirty();
    } else if (e == InputEvent::Select) {
      if (clock_tab_ == 1) {
        focus_.index = 0;
        nav_.push(ScreenId::ClockAlarms);
        after_nav();
      } else if (clock_tab_ == 2) {
        focus_.index = 0;
        nav_.push(timer_running_ ? ScreenId::ClockTimerRun : ScreenId::ClockTimers);
        after_nav();
      }
    }
    return;
  }

  if (s == ScreenId::ClockAlarms) {
    const int n = static_cast<int>(data_.alarms.size());
    focus_.count = n + 2;
    if (e == InputEvent::Back) {
      nav_.pop();
      after_nav();
    } else if (rot) {
      const int prev = focus_.index;
      focus_.move(dir);
      if (focus_.index != prev) mark_content_dirty();
    } else if (e == InputEvent::Select) {
      if (focus_.index < n) {
        open_alarm_editor(focus_.index);
      } else if (focus_.index == n) {
        if (n >= 8) {
          error_msg_ = "Up to 8 alarms.";
          error_until_ms_ = now_ms_ + 2500;
          transient_error_armed_ = true;
          mark_content_dirty();
        } else {
          open_alarm_editor(-1);
        }
      } else {
        nav_.pop();
        after_nav();
      }
    }
    return;
  }

  if (s == ScreenId::ClockAlarmEdit) {
    const bool existing = alarm_edit_index_ >= 0;
    focus_.count = existing ? 5 : 4;
    if (alarm_adjust_ != 0) {
      if (rot) {
        int mins = alarm_draft_.minutes_of_day;
        if (alarm_adjust_ == 1) mins = (mins + dir * 60 + 1440) % 1440;
        else mins = (mins / 60) * 60 + ((mins % 60) + dir + 60) % 60;
        alarm_draft_.minutes_of_day = mins;
        mark_content_dirty();
      } else if (e == InputEvent::Select || e == InputEvent::Back) {
        alarm_adjust_ = 0;
        mark_content_dirty();
      }
      return;
    }
    if (e == InputEvent::Back) {
      nav_.pop();
      after_nav();
      return;
    }
    if (rot) {
      const int prev = focus_.index;
      focus_.move(dir);
      if (focus_.index != prev) mark_content_dirty();
      return;
    }
    if (e != InputEvent::Select) return;
    const int save_i = existing ? 4 : 3;
    if (focus_.index == 0 || focus_.index == 1) {
      alarm_adjust_ = focus_.index + 1;
      mark_content_dirty();
    } else if (focus_.index == 2) {
      alarm_draft_.enabled = !alarm_draft_.enabled;
      mark_content_dirty();
    } else if (existing && focus_.index == 3) {
      data_.alarms.erase(data_.alarms.begin() + alarm_edit_index_);
      save_alarms();
      focus_.index = 0;
      nav_.pop();
      after_nav();
    } else if (focus_.index == save_i) {
      if (existing && alarm_edit_index_ < static_cast<int>(data_.alarms.size())) {
        data_.alarms[static_cast<size_t>(alarm_edit_index_)] = alarm_draft_;
      } else {
        data_.alarms.push_back(alarm_draft_);
      }
      save_alarms();
      play_sound(SoundId::Success);
      focus_.index = 0;
      nav_.pop();
      after_nav();
    }
    return;
  }

  if (s == ScreenId::ClockTimers) {
    focus_.count = kTimerPresetCount + 1;
    if (e == InputEvent::Back) {
      nav_.pop();
      after_nav();
    } else if (rot) {
      const int prev = focus_.index;
      focus_.move(dir);
      if (focus_.index != prev) mark_list_focus_dirty(kListTop, prev, focus_.index);
    } else if (e == InputEvent::Select) {
      if (focus_.index >= kTimerPresetCount) {
        nav_.pop();
        after_nav();
        return;
      }
      timer_total_ms_ = static_cast<uint32_t>(kTimerPresetsMin[focus_.index]) * 60000u;
      timer_end_ms_ = now_ms_ + timer_total_ms_;
      timer_running_ = true;
      timer_paused_ = false;
      timer_shown_s_ = -1;
      play_sound(SoundId::Click);
      focus_.index = 0;
      nav_.replace(ScreenId::ClockTimerRun);
      after_nav();
    }
    return;
  }

  if (s == ScreenId::ClockTimerRun) {
    focus_.count = 2;
    if (e == InputEvent::Back) {
      nav_.pop();  // timer keeps running in the background
      after_nav();
    } else if (rot) {
      const int prev = focus_.index;
      focus_.move(dir);
      if (focus_.index != prev) mark_content_dirty();
    } else if (e == InputEvent::Select) {
      if (focus_.index == 0) {
        if (timer_paused_) {
          timer_end_ms_ = now_ms_ + timer_paused_left_ms_;
          timer_paused_ = false;
        } else {
          timer_paused_left_ms_ = timer_remaining_ms();
          timer_paused_ = true;
        }
        mark_content_dirty();
      } else {
        timer_running_ = false;
        timer_paused_ = false;
        focus_.index = 0;
        nav_.replace(ScreenId::ClockTimers);
        after_nav();
      }
    }
    return;
  }
}

}  // namespace pocket
