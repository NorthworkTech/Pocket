/** PocketOS 2.3: calendar on Today, battery saver, bedtime face, backup & restore. */
#include "pocket/app.hpp"
#include "pocket/json_lite.hpp"
#include "pocket/ui.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace pocket {

namespace {

/** Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

/** "2026-11-19T15:30:00.000Z" / "+02:00" offsets → unix seconds (0 on failure). */
int64_t parse_iso(const std::string& s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) < 5) {
    if (std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &mo, &d) != 3) return 0;
  }
  int64_t t = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 + se;
  const size_t tpos = s.find('T');
  if (tpos != std::string::npos) {
    const size_t z = s.find_first_of("+-", tpos);
    if (z != std::string::npos && s.size() >= z + 6) {
      const int oh = std::atoi(s.substr(z + 1, 2).c_str()), om = std::atoi(s.substr(z + 4, 2).c_str());
      t -= (s[z] == '-' ? -1 : 1) * (oh * 3600 + om * 60);
    }
  }
  return t;
}

std::string hm(const struct tm& t, int fmt) {
  char b[16];
  if (fmt == 24) std::snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
  else std::snprintf(b, sizeof(b), "%d:%02d %s", t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  return b;
}

std::string esc(const std::string& s) { return json_escape_str(s); }

}  // namespace

// ---------------------------------------------------------------- Calendar

void App::calendar_load_cache() {
  if (!storage_ || !storage_->data_ensure_root()) return;
  std::string body;
  if (!read_file_recover(storage_->data_root() + "/calendar.json", body)) return;
  JsonValue v;
  if (!json_parse(body, v) || !v.is_object()) return;
  const JsonValue* ev = v.get("events");
  if (!ev || !ev->is_array()) return;
  calendar_.clear();
  for (const auto& o : ev->items) {
    if (!o.is_object()) continue;
    CalEvent e;
    e.title = o.get_string("title");
    e.location = o.get_string("location");
    e.start = parse_iso(o.get_string("start"));
    const JsonValue* en = o.get("end");
    e.end = en && en->type == JsonValue::Type::String ? parse_iso(en->str) : 0;
    e.all_day = o.get_bool("all_day");
    if (e.start > 0 && !e.title.empty()) calendar_.push_back(std::move(e));
  }
  std::sort(calendar_.begin(), calendar_.end(), [](const CalEvent& a, const CalEvent& b) { return a.start < b.start; });
}

void App::maybe_fetch_calendar(bool force) {
  if (!cfg_.companion_linked || !wifi_.connected() || cfg_.device_id.empty() || !storage_) return;
  const uint32_t every = battery_saver_ ? 60u * 60u * 1000u : 15u * 60u * 1000u;
  if (!force && calendar_fetch_ms_ != 0 && now_ms_ - calendar_fetch_ms_ < every) return;
  calendar_fetch_ms_ = now_ms_;
  const std::string body = cloud_.calendar_json(cfg_.device_id);
  JsonValue v;
  if (body.empty() || !json_parse(body, v) || !v.is_object() || !v.get("events")) return;
  if (storage_->data_ensure_root()) (void)write_file_atomic(storage_->data_root() + "/calendar.json", body);
  calendar_load_cache();
}

bool App::next_calendar_event(std::string* line, std::string* meta) const {
  if (!clock_.time_valid()) return false;
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  // Local calendar date today, for all-day events (sent as UTC dates by the server).
  struct tm today {};
  {
    const time_t nt = static_cast<time_t>(now);
    localtime_r(&nt, &today);
  }
  const int64_t today_key = (today.tm_year + 1900) * 10000 + (today.tm_mon + 1) * 100 + today.tm_mday;
  for (const auto& e : calendar_) {
    if (e.all_day) {
      const time_t st = static_cast<time_t>(e.start);
      struct tm d {};
      gmtime_r(&st, &d);
      const int64_t key = (d.tm_year + 1900) * 10000 + (d.tm_mon + 1) * 100 + d.tm_mday;
      if (key < today_key) continue;  // an all-day event lasts the whole local day
    } else {
      const int64_t end = e.end > e.start ? e.end : e.start + 3600;
      if (end <= now) continue;
    }
    const time_t st = static_cast<time_t>(e.start), nt = static_cast<time_t>(now);
    struct tm s {}, n {};
    if (e.all_day) gmtime_r(&st, &s);  // all-day dates are calendar dates, not instants
    else localtime_r(&st, &s);
    localtime_r(&nt, &n);
    struct tm a = s, b = n;
    a.tm_hour = b.tm_hour = 12;
    a.tm_min = b.tm_min = a.tm_sec = b.tm_sec = 0;
    a.tm_isdst = b.tm_isdst = -1;
    const long days = std::lround(std::difftime(std::mktime(&a), std::mktime(&b)) / 86400.0);
    static const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    std::string when = days <= 0 ? "Today" : days == 1 ? "Tomorrow" : kDays[s.tm_wday];
    if (e.start <= now && !e.all_day) when = "Now";
    if (line) *line = (e.all_day ? std::string("All day") : hm(s, cfg_.time_format)) + " \xC2\xB7 " + e.title;
    if (meta) *meta = when + (e.location.empty() ? std::string() : " \xC2\xB7 " + e.location);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- Battery saver

void App::maybe_battery_saver() {
  if (!cfg_.onboarding_complete) return;
  if (last_saver_check_ms_ != 0 && now_ms_ - last_saver_check_ms_ < 30000u) return;
  last_saver_check_ms_ = now_ms_;
  const int b = clock_.battery_percent();
  if (b < 0) return;
  if (!battery_saver_ && b <= 15) {
    battery_saver_ = true;
    if (nav_.current() != ScreenId::Lock && nav_.current() != ScreenId::Pin) {
      error_msg_ = "Battery saver on \xE2\x80\x94 Pocket checks in less often until you charge.";
      error_until_ms_ = now_ms_ + 5000;
      transient_error_armed_ = true;
    }
    mark_status_dirty();
  } else if (battery_saver_ && b >= 20) {
    battery_saver_ = false;
    mark_status_dirty();
  }
}

// ---------------------------------------------------------------- Bedtime face

void App::render_night_face() {
  // A calm night card during quiet hours: dark sky, a crescent, a few stars,
  // and when the morning starts. E-ink holds it with no power.
  canvas_.fill_rect(0, 0, kCanvasW, kCanvasH, Gray::G0);
  uint32_t seed = 0x9E3779B9u;
  for (int i = 0; i < 46; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const int x = static_cast<int>(seed % kCanvasW);
    seed = seed * 1664525u + 1013904223u;
    const int y = 40 + static_cast<int>(seed % 420);
    const int r = (seed >> 20) % 7 == 0 ? 2 : 1;
    canvas_.fill_disc(x, y, r, Gray::G3);
  }
  // Crescent: a white disc with a black disc bitten out of it.
  canvas_.fill_disc(kCanvasW / 2 + 10, 250, 72, Gray::G3);
  canvas_.fill_disc(kCanvasW / 2 + 44, 226, 66, Gray::G0);
  canvas_.draw_text_centered(kCanvasW / 2, 420, "Good night", Canvas::TextRole::ScreenTitle, Gray::G3);
  // Tomorrow's first alarm, or when quiet hours end.
  std::string line;
  const Alarm* first = nullptr;
  for (const auto& a : data_.alarms)
    if (a.enabled && (!first || a.minutes_of_day < first->minutes_of_day)) first = &a;
  auto fmt = [&](int mins) {
    struct tm t {};
    t.tm_hour = mins / 60;
    t.tm_min = mins % 60;
    return hm(t, cfg_.time_format);
  };
  if (first) line = "Alarm " + fmt(first->minutes_of_day);
  else line = "No alarm set";
  canvas_.draw_text_centered(kCanvasW / 2, 488, line, Canvas::TextRole::BodyStrong, Gray::G3);
  canvas_.draw_text_centered(kCanvasW / 2, 530, "Quiet until " + fmt(cfg_.quiet_end_min), Canvas::TextRole::Caption,
                             Gray::G3);
  canvas_.draw_text_centered(kCanvasW / 2, kCanvasH - 70, "Press to unlock", Canvas::TextRole::Caption, Gray::G3);
}

// ---------------------------------------------------------------- Backup & restore

std::string App::backup_payload() const {
  const DeviceConfig& c = cfg_;
  char num[512];
  std::snprintf(num, sizeof(num),
                "\"time_format\":%d,\"weather_units\":%d,\"weather_lat\":%.4f,\"weather_lon\":%.4f,"
                "\"idle_lock_s\":%d,\"show_batt_pct\":%s,\"lock_info\":%s,\"screensaver\":%d,"
                "\"brightness_percent\":%d,\"volume_percent\":%d,\"quiet_enabled\":%s,\"quiet_start_min\":%d,"
                "\"quiet_end_min\":%d,\"home_visible\":%d,\"ghosting_frequent\":%s",
                c.time_format, c.weather_units, c.weather_lat, c.weather_lon, c.idle_lock_s,
                c.show_batt_pct ? "true" : "false", c.lock_info ? "true" : "false", c.screensaver,
                c.brightness_percent, c.volume_percent, c.quiet_enabled ? "true" : "false", c.quiet_start_min,
                c.quiet_end_min, c.home_visible, c.ghosting_frequent ? "true" : "false");
  std::string home = "[";
  for (size_t i = 0; i < c.home_order.size(); ++i) home += (i ? "," : "") + std::to_string(c.home_order[i]);
  home += "]";
  std::string out = "{\"settings\":{\"tz_id\":\"" + esc(c.tz_id) + "\",\"weather_city\":\"" + esc(c.weather_city) +
                    "\",\"lock_message\":\"" + esc(c.lock_message) + "\",\"device_name\":\"" + esc(c.device_name) +
                    "\",\"home_order\":" + home + "," + num + "},\"alarms\":[";
  for (size_t i = 0; i < data_.alarms.size(); ++i) {
    const auto& a = data_.alarms[i];
    out += std::string(i ? "," : "") + "{\"id\":\"" + esc(a.id) + "\",\"minutes\":" + std::to_string(a.minutes_of_day) +
           ",\"enabled\":" + (a.enabled ? "true" : "false") + ",\"label\":\"" + esc(a.label) + "\"}";
  }
  out += "],\"reading\":{\"positions\":[";
  if (!c.reading_last_book_id.empty())
    out += "{\"book\":\"" + esc(c.reading_last_book_id) + "\",\"page\":" + std::to_string(c.reading_last_page) + "}";
  out += "]}}";
  return out;
}

void App::maybe_backup(bool force) {
  if (!cfg_.onboarding_complete || !cfg_.companion_linked || !wifi_.connected() || cfg_.device_id.empty()) return;
  if (!clock_.time_valid()) return;
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (!force && !backup_requested_ && last_backup_unix_ != 0 && now - last_backup_unix_ < 24 * 3600) return;
  if (!force && !backup_requested_ && last_backup_attempt_ms_ != 0 && now_ms_ - last_backup_attempt_ms_ < 600000u)
    return;  // a failing server is retried every 10 minutes, not every tick
  last_backup_attempt_ms_ = now_ms_;
  if (cloud_.post_backup(cfg_.device_id, backup_payload())) {
    last_backup_unix_ = now;
    backup_requested_ = false;
    if (storage_ && storage_->data_ensure_root())
      (void)write_file_atomic(storage_->data_root() + "/backup_state.json",
                              "{\"last\":" + std::to_string(last_backup_unix_) + "}");
  }
}

bool App::apply_backup(const std::string& backup_json) {
  JsonValue root;
  if (!json_parse(backup_json, root) || !root.is_object()) return false;
  const JsonValue* b = root.get("backup");
  if (!b || !b->is_object()) b = &root;
  if (const JsonValue* s = b->get("settings"); s && s->is_object()) {
    DeviceConfig& c = cfg_;
    // Never restored: PIN, Wi-Fi passwords, device identity, linking.
    auto str = [&](const char* k, std::string& dst) {
      if (const JsonValue* v = s->get(k); v && v->type == JsonValue::Type::String) dst = v->str;
    };
    auto num = [&](const char* k, auto& dst, double lo, double hi) {
      if (const JsonValue* v = s->get(k); v && v->type == JsonValue::Type::Number && v->number >= lo && v->number <= hi)
        dst = static_cast<std::remove_reference_t<decltype(dst)>>(v->number);
    };
    auto boolean = [&](const char* k, bool& dst) {
      if (const JsonValue* v = s->get(k); v && v->type == JsonValue::Type::Bool) dst = v->boolean;
    };
    str("tz_id", c.tz_id);
    str("weather_city", c.weather_city);
    str("lock_message", c.lock_message);
    num("time_format", c.time_format, 12, 24);
    num("weather_units", c.weather_units, 0, 1);
    num("weather_lat", c.weather_lat, -90, 90);
    num("weather_lon", c.weather_lon, -180, 180);
    num("idle_lock_s", c.idle_lock_s, 10, 3600);
    boolean("show_batt_pct", c.show_batt_pct);
    boolean("lock_info", c.lock_info);
    num("screensaver", c.screensaver, 0, 255);
    num("brightness_percent", c.brightness_percent, 0, 100);
    num("volume_percent", c.volume_percent, 0, 100);
    boolean("quiet_enabled", c.quiet_enabled);
    num("quiet_start_min", c.quiet_start_min, 0, 1439);
    num("quiet_end_min", c.quiet_end_min, 0, 1439);
    num("home_visible", c.home_visible, 0, 65535);
    boolean("ghosting_frequent", c.ghosting_frequent);
    if (const JsonValue* h = s->get("home_order"); h && h->is_array()) {
      c.home_order.clear();
      for (const auto& it : h->items)
        if (it.type == JsonValue::Type::Number && it.number >= 0 && it.number < kHomeGridSlots && c.home_order.size() < 16)
          c.home_order.push_back(static_cast<uint8_t>(it.number));
    }
  }
  if (const JsonValue* al = b->get("alarms"); al && al->is_array()) {
    data_.alarms.clear();
    for (const auto& o : al->items) {
      if (!o.is_object()) continue;
      Alarm a;
      a.id = o.get_string("id");
      a.minutes_of_day = static_cast<int>(o.get_int("minutes"));
      a.enabled = o.get_bool("enabled");
      a.label = o.get_string("label");
      if (!a.id.empty() && a.minutes_of_day >= 0 && a.minutes_of_day < 1440) data_.alarms.push_back(a);
    }
    save_alarms();
  }
  if (const JsonValue* r = b->get("reading"); r && r->is_object()) {
    const JsonValue* pos = r->get("positions");
    const JsonValue* p0 = (pos && pos->is_array() && !pos->items.empty() && pos->items[0].is_object())
                              ? &pos->items[0]
                              : r;  // pre-2.3 shape {book,page}
    if (!p0->get_string("book").empty()) {
      cfg_.reading_last_book_id = p0->get_string("book");
      cfg_.reading_last_page = static_cast<int>(p0->get_int("page"));
    }
  }
  if (audio_) audio_->set_volume(cfg_.volume_percent);
  display_.set_brightness(cfg_.brightness_percent);
  store_.save(cfg_);
  return true;
}

void App::render_restore() {
  draw_status_bar();
  ui::header(canvas_, "Welcome back");
  ui::empty_state(canvas_, ui::Icon::Cloud, "Restore from a backup?",
                  "Bring back your settings, alarms and reading places. Notes and lists come back from Pocket Cloud "
                  "either way.",
                  330);
  if (!restore_offer_label_.empty())
    canvas_.draw_text_centered(kCanvasW / 2, 470, restore_offer_label_, Canvas::TextRole::Caption, Gray::G0);
  focus_.count = 2;
  ui::button(canvas_, kBottomCtaY - kRowPitch, "Restore", focus_.index == 0, ui::Icon::Update, true);
  ui::row(canvas_, kBottomCtaY, ui::Row{"Start fresh", ui::RowKind::Plain}, focus_.index == 1);
}

void App::handle_restore(InputEvent e) {
  focus_.count = 2;
  if (e == InputEvent::Up || e == InputEvent::Down) {
    focus_.move(e == InputEvent::Down ? 1 : -1);
    mark_content_dirty();
    return;
  }
  if (e != InputEvent::Select) return;
  if (focus_.index == 0) {
    play_sound(apply_backup(restore_offer_json_) ? SoundId::Success : SoundId::Attention);
  } else {
    play_sound(SoundId::Click);
  }
  restore_offer_json_.clear();
  focus_.index = 0;
  nav_.replace(ScreenId::OnboardingPinLength);
  after_nav();
}

bool App::offer_restore_if_any() {
  if (!wifi_.connected() || cfg_.device_id.empty()) return false;
  const std::string body = cloud_.backup_latest_json(cfg_.device_id);
  JsonValue v;
  if (body.empty() || !json_parse(body, v) || !v.is_object()) return false;
  const JsonValue* b = v.get("backup");
  if (!b || !b->is_object()) return false;
  restore_offer_json_ = body;
  const std::string when = b->get_string("created_at");
  const std::string from = b->get_string("device_name");
  restore_offer_label_ = "From " + (from.empty() ? std::string("your Pocket") : from) +
                         (when.size() >= 10 ? " \xC2\xB7 " + when.substr(0, 10) : std::string());
  return true;
}

}  // namespace pocket
