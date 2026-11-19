/** Reading stats that learn your speed: each forward page turn after a real read (not a
 * flick, not a coffee break) is a words-per-minute sample; a smoothed average turns the
 * words left in the book into "about 2 h 10 min left". Time read per day and per book is
 * kept for "Today 25 min · This week 2 h 40 min". Stored in reading_stats.json. */
#include "pocket/app.hpp"
#include "pocket/json_lite.hpp"
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace pocket {

namespace {

size_t count_words(const std::string& t, size_t a, size_t b) {
  size_t n = 0;
  bool in = false;
  for (size_t i = a; i < b && i < t.size(); ++i) {
    const bool ws = t[i] == ' ' || t[i] == '\n' || t[i] == '\t' || t[i] == '\r';
    if (!ws && !in) ++n;
    in = !ws;
  }
  return n;
}

std::string fmt_duration(int minutes) {
  if (minutes < 1) return "under a minute";
  if (minutes < 60) return std::to_string(minutes) + " min";
  const int h = minutes / 60, m = minutes % 60;
  return std::to_string(h) + " h" + (m ? " " + std::to_string(m) + " min" : std::string());
}

}  // namespace

std::string App::reading_day_key(int days_ago) const {
  if (!clock_.time_valid()) return {};
  const time_t t = std::time(nullptr) - static_cast<time_t>(days_ago) * 86400;
  struct tm lt {};
  localtime_r(&t, &lt);
  char b[16];
  std::strftime(b, sizeof(b), "%Y-%m-%d", &lt);
  return b;
}

void App::reading_stats_load() {
  if (reading_stats_loaded_) return;
  reading_stats_loaded_ = true;
  if (!storage_ || !storage_->data_ensure_root()) return;
  std::string body;
  if (!read_file_recover(storage_->data_root() + "/reading_stats.json", body)) return;
  JsonValue v;
  if (!json_parse(body, v) || !v.is_object()) return;
  reading_wpm_ = static_cast<float>(v.get_number("wpm", 0));
  reading_wpm_samples_ = static_cast<int>(v.get_int("samples", 0));
  if (const JsonValue* d = v.get("days"); d && d->is_object())
    for (const auto& m : d->members)
      if (m.second.type == JsonValue::Type::Number) reading_day_seconds_[m.first] = static_cast<int>(m.second.number);
}

void App::reading_stats_save() {
  if (!storage_ || !storage_->data_ensure_root()) return;
  // Keep two weeks of days.
  const std::string oldest = reading_day_key(14);
  for (auto it = reading_day_seconds_.begin(); it != reading_day_seconds_.end();)
    it = (!oldest.empty() && it->first < oldest) ? reading_day_seconds_.erase(it) : std::next(it);
  char head[96];
  std::snprintf(head, sizeof(head), "{\"wpm\":%.1f,\"samples\":%d,\"days\":{", reading_wpm_, reading_wpm_samples_);
  std::string out = head;
  bool first = true;
  for (const auto& kv : reading_day_seconds_) {
    out += std::string(first ? "" : ",") + "\"" + kv.first + "\":" + std::to_string(kv.second);
    first = false;
  }
  out += "}}";
  (void)write_file_atomic(storage_->data_root() + "/reading_stats.json", out);
}

void App::reading_note_page_left(bool forward) {
  reading_stats_load();
  const uint32_t now = now_ms_;
  const uint32_t dwell = reading_page_shown_ms_ ? now - reading_page_shown_ms_ : 0;
  reading_page_shown_ms_ = now;
  if (dwell < 1500 || reading_pages_.empty()) return;
  // Reading time: cap one page at 5 minutes so a long pause doesn't count as reading.
  const int secs = static_cast<int>(std::min<uint32_t>(dwell, 5u * 60u * 1000u) / 1000u);
  const std::string today = reading_day_key(0);
  if (!today.empty()) reading_day_seconds_[today] += secs;
  // Speed sample: only forward turns of a real page read at a plausible pace.
  if (forward && dwell >= 4000 && dwell <= 10u * 60u * 1000u) {
    const size_t p = static_cast<size_t>(std::clamp(reading_page_, 0, static_cast<int>(reading_pages_.size()) - 1));
    const size_t a = reading_pages_[p];
    const size_t b = p + 1 < reading_pages_.size() ? reading_pages_[p + 1] : reading_text_.size();
    const size_t words = count_words(reading_text_, a, b);
    if (words >= 20) {
      const float sample = static_cast<float>(words) / (static_cast<float>(dwell) / 60000.0f);
      if (sample >= 60.0f && sample <= 900.0f) {
        // Running mean for the first few, then a slow EWMA that follows how you read now.
        if (reading_wpm_samples_ < 5)
          reading_wpm_ = (reading_wpm_ * reading_wpm_samples_ + sample) / (reading_wpm_samples_ + 1);
        else
          reading_wpm_ = 0.85f * reading_wpm_ + 0.15f * sample;
        ++reading_wpm_samples_;
      }
    }
  }
  if (++reading_turns_since_save_ >= 5) {
    reading_turns_since_save_ = 0;
    reading_stats_save();
  }
}

int App::reading_minutes_left() const {
  if (reading_wpm_samples_ < 3 || reading_wpm_ < 1 || reading_pages_.empty()) return -1;
  const size_t p = static_cast<size_t>(std::clamp(reading_page_, 0, static_cast<int>(reading_pages_.size()) - 1));
  const size_t words = count_words(reading_text_, reading_pages_[p], reading_text_.size());
  return static_cast<int>(static_cast<float>(words) / reading_wpm_ + 0.5f);
}

std::string App::reading_time_left_label() const {
  const int m = reading_minutes_left();
  if (m < 0) return reading_wpm_samples_ > 0 ? "learning your pace" : std::string();
  return "about " + fmt_duration(m) + " left";
}

std::string App::reading_summary_line() {
  reading_stats_load();
  int today = 0, week = 0;
  for (int d = 0; d < 7; ++d) {
    const auto it = reading_day_seconds_.find(reading_day_key(d));
    if (it == reading_day_seconds_.end()) continue;
    if (d == 0) today = it->second;
    week += it->second;
  }
  if (week < 60) return {};
  std::string s = "Today " + fmt_duration(today / 60) + " \xC2\xB7 This week " + fmt_duration(week / 60);
  if (reading_wpm_samples_ >= 3) s += " \xC2\xB7 " + std::to_string(static_cast<int>(reading_wpm_ + 0.5f)) + " wpm";
  return s;
}

}  // namespace pocket
