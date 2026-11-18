#include "pocket/app.hpp"
#include "pocket/ui.hpp"
#include "pocket/json_lite.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace pocket {
namespace {

}  // namespace

namespace {

/** Parse a stored index file; on corruption keep a copy so the next save can't erase it. */
bool load_index(const std::string& path, JsonValue& root) {
  std::string text;
  if (!read_file_recover(path, text)) return false;
  if (json_parse(text, root) && root.is_array()) return true;
  write_file_atomic(path + ".corrupt", text);
  return false;
}

}  // namespace

void App::load_local_notes() {
  data_.notes.clear();
  if (!storage_ || !storage_->data_ensure_root()) return;
  JsonValue root;
  if (!load_index(storage_->data_root() + "/notes/index.json", root)) return;
  for (const auto& o : root.items) {
    if (!o.is_object()) continue;
    Note n;
    n.id = o.get_string("id");
    n.title = o.get_string("title");
    n.body = o.get_string("body");
    n.created_at = o.get_int("created_at");
    n.updated_at = o.get_int("updated_at");
    n.pending_pcm = o.get_string("pending");
    if (!n.id.empty()) data_.notes.push_back(std::move(n));
  }
}

bool App::save_local_notes() {
  if (!storage_ || !storage_->data_ensure_root()) {
    error_msg_ = "Can't save notes — no local storage.";
    error_until_ms_ = now_ms_ + 3000;
    return false;
  }
  const std::string dir = storage_->data_root() + "/notes";
  ::mkdir(dir.c_str(), 0755);
  std::string out = "[";
  for (size_t i = 0; i < data_.notes.size(); ++i) {
    const auto& n = data_.notes[i];
    if (i) out += ",";
    out += "{\"id\":\"" + json_escape_str(n.id) + "\",\"title\":\"" + json_escape_str(n.title) + "\",\"body\":\"" +
           json_escape_str(n.body) + "\",\"created_at\":" + std::to_string(n.created_at) +
           ",\"updated_at\":" + std::to_string(n.updated_at);
    if (!n.pending_pcm.empty()) out += ",\"pending\":\"" + json_escape_str(n.pending_pcm) + "\"";
    out += "}";
  }
  out += "]";
  if (!write_file_atomic(dir + "/index.json", out)) {
    error_msg_ = "Couldn't write notes to local storage.";
    error_until_ms_ = now_ms_ + 3000;
    return false;
  }
  return true;
}

void App::load_local_lists() {
  data_.lists.clear();
  if (!storage_ || !storage_->data_ensure_root()) return;
  JsonValue root;
  if (!load_index(storage_->data_root() + "/lists/index.json", root)) return;
  for (const auto& o : root.items) {
    if (!o.is_object()) continue;
    TodoList L;
    L.id = o.get_string("id");
    L.title = o.get_string("title");
    L.updated_at = o.get_int("updated_at");
    if (const JsonValue* items = o.get("items"); items && items->is_array()) {
      for (const auto& io : items->items) {
        if (!io.is_object()) continue;
        ListItem it;
        it.id = io.get_string("id");
        it.text = io.get_string("text");
        it.checked = io.get_bool("checked");
        it.pending_pcm = io.get_string("pending");
        if (!it.text.empty() || !it.pending_pcm.empty()) L.items.push_back(std::move(it));
      }
    }
    if (!L.id.empty()) data_.lists.push_back(std::move(L));
  }
}

bool App::save_local_lists() {
  if (!storage_ || !storage_->data_ensure_root()) {
    error_msg_ = "Can't save lists — no local storage.";
    error_until_ms_ = now_ms_ + 3000;
    return false;
  }
  const std::string dir = storage_->data_root() + "/lists";
  ::mkdir(dir.c_str(), 0755);
  std::string out = "[";
  for (size_t i = 0; i < data_.lists.size(); ++i) {
    const auto& L = data_.lists[i];
    if (i) out += ",";
    out += "{\"id\":\"" + json_escape_str(L.id) + "\",\"title\":\"" + json_escape_str(L.title) +
           "\",\"updated_at\":" + std::to_string(L.updated_at) + ",\"items\":[";
    for (size_t j = 0; j < L.items.size(); ++j) {
      const auto& it = L.items[j];
      if (j) out += ",";
      out += "{\"id\":\"" + json_escape_str(it.id) + "\",\"text\":\"" + json_escape_str(it.text) +
             "\",\"checked\":" + (it.checked ? "true" : "false");
      if (!it.pending_pcm.empty()) out += ",\"pending\":\"" + json_escape_str(it.pending_pcm) + "\"";
      out += "}";
    }
    out += "]}";
  }
  out += "]";
  if (!write_file_atomic(dir + "/index.json", out)) {
    error_msg_ = "Couldn't write lists to local storage.";
    error_until_ms_ = now_ms_ + 3000;
    return false;
  }
  return true;
}

void App::load_local_passes() {
  data_.passes.clear();
  if (!storage_ || !storage_->data_ensure_root()) return;
  JsonValue root;
  if (!load_index(storage_->data_root() + "/passes/index.json", root)) return;
  for (const auto& o : root.items) {
    if (!o.is_object()) continue;
    Pass p;
    p.id = o.get_string("id");
    p.title = o.get_string("title");
    p.type = o.get_string("type");
    p.payload = o.get_string("payload");
    if (p.id.empty()) continue;
    if (p.title.empty()) p.title = p.id;
    data_.passes.push_back(std::move(p));
  }
}

void App::save_local_passes() {
  if (!storage_ || !storage_->data_ensure_root()) return;
  const std::string dir = storage_->data_root() + "/passes";
  ::mkdir(dir.c_str(), 0755);
  std::string out = "[";
  for (size_t i = 0; i < data_.passes.size(); ++i) {
    const auto& p = data_.passes[i];
    if (i) out += ",";
    out += "{\"id\":\"" + json_escape_str(p.id) + "\",\"title\":\"" + json_escape_str(p.title) + "\",\"type\":\"" +
           json_escape_str(p.type) + "\",\"payload\":\"" + json_escape_str(p.payload) + "\"}";
  }
  out += "]";
  write_file_atomic(dir + "/index.json", out);
}

static const char* wmo_condition(int code) {
  if (code == 0) return "Clear";
  if (code <= 3) return "Partly cloudy";
  if (code <= 48) return "Fog";
  if (code <= 57) return "Drizzle";
  if (code <= 67) return "Rain";
  if (code <= 77) return "Snow";
  if (code <= 82) return "Showers";
  if (code <= 86) return "Snow showers";
  if (code <= 99) return "Thunderstorm";
  return "—";
}

void App::load_weather_cache() {
  data_.weather = WeatherCache{};
  if (!storage_ || !storage_->data_ensure_root()) {
    if (!cfg_.weather_city.empty()) data_.weather.city = cfg_.weather_city;
    data_.weather.units = cfg_.weather_units;
    return;
  }
  std::string text;
  JsonValue root;
  if (!read_file_recover(storage_->data_root() + "/weather/cache.json", text) || !json_parse(text, root) ||
      !root.is_object()) {
    if (!cfg_.weather_city.empty()) data_.weather.city = cfg_.weather_city;
    data_.weather.units = cfg_.weather_units;
    return;
  }
  data_.weather.city = root.get_string("city");
  data_.weather.fetched_at = root.get_int("fetched_at");
  data_.weather.units = static_cast<uint8_t>(root.get_int("units", cfg_.weather_units));
  data_.weather.today_temp = static_cast<int>(root.get_int("today_temp"));
  data_.weather.today_condition = root.get_string("today_condition");
  data_.weather.fetched_hm = static_cast<int>(root.get_int("fetched_hm", -1));
  data_.weather.days.clear();
  if (const JsonValue* days = root.get("days"); days && days->is_array()) {
    for (const auto& o : days->items) {
      if (!o.is_object()) continue;
      WeatherDay d;
      d.date = o.get_string("date");
      d.hi = static_cast<int>(o.get_int("hi"));
      d.lo = static_cast<int>(o.get_int("lo"));
      d.condition = o.get_string("condition");
      if (!d.date.empty()) data_.weather.days.push_back(std::move(d));
      if (data_.weather.days.size() >= 5) break;
    }
  }
  if (data_.weather.city.empty() && !cfg_.weather_city.empty()) data_.weather.city = cfg_.weather_city;
  if (data_.weather.units == 0 && cfg_.weather_units) data_.weather.units = cfg_.weather_units;
}

void App::save_weather_cache() {
  if (!storage_ || !storage_->data_ensure_root()) return;
  const std::string dir = storage_->data_root() + "/weather";
  ::mkdir(dir.c_str(), 0755);
  std::string out = "{\"city\":\"" + json_escape_str(data_.weather.city) +
                    "\",\"units\":" + std::to_string(static_cast<int>(data_.weather.units)) +
                    ",\"fetched_at\":" + std::to_string(data_.weather.fetched_at) +
                    ",\"today_temp\":" + std::to_string(data_.weather.today_temp) +
                    ",\"fetched_hm\":" + std::to_string(data_.weather.fetched_hm) + ",\"today_condition\":\"" +
                    json_escape_str(data_.weather.today_condition) + "\",\"days\":[";
  for (size_t i = 0; i < data_.weather.days.size(); ++i) {
    const auto& d = data_.weather.days[i];
    if (i) out += ",";
    out += "{\"date\":\"" + json_escape_str(d.date) + "\",\"hi\":" + std::to_string(d.hi) +
           ",\"lo\":" + std::to_string(d.lo) + ",\"condition\":\"" + json_escape_str(d.condition) + "\"}";
  }
  out += "]}";
  write_file_atomic(dir + "/cache.json", out);
}

bool App::save_pending_pcm(const std::vector<uint8_t>& pcm, std::string* out_name) {
  if (!out_name || !storage_ || !storage_->data_ensure_root() || pcm.empty()) return false;
  const std::string notes_dir = storage_->data_root() + "/notes";
  const std::string dir = notes_dir + "/pcm";
  ::mkdir(notes_dir.c_str(), 0755);
  ::mkdir(dir.c_str(), 0755);
  if (storage_->free_bytes(storage_->data_root()) < pcm.size() + 32768ull) {
    error_msg_ = "Not enough space to save voice clip.";
    error_until_ms_ = now_ms_ + 3500;
    return false;
  }
  const std::string name = std::to_string(now_ms_) + "-" + std::to_string(pcm.size()) + ".pcm";
  const std::string path = dir + "/" + name;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    error_msg_ = "Couldn't save voice clip.";
    error_until_ms_ = now_ms_ + 3000;
    return false;
  }
  out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
  if (!out) {
    error_msg_ = "Couldn't save voice clip.";
    error_until_ms_ = now_ms_ + 3000;
    ::unlink(path.c_str());
    return false;
  }
  *out_name = name;
  return true;
}

void App::delete_pending_pcm(const std::string& name) {
  if (!storage_ || name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos)
    return;
  const std::string path = storage_->data_root() + "/notes/pcm/" + name;
  ::unlink(path.c_str());
}

bool App::notes_have_pending_dictation() const {
  for (const auto& n : data_.notes) {
    if (!n.pending_pcm.empty()) return true;
  }
  for (const auto& L : data_.lists) {
    for (const auto& it : L.items) {
      if (!it.pending_pcm.empty()) return true;
    }
  }
  return false;
}

void App::apply_dictation_transcript(const std::string& t) {
  const ScreenId s = nav_.current();
  if (s == ScreenId::ListsDetail && note_index_ < static_cast<int>(data_.lists.size())) {
    ListItem it;
    it.id = new_local_id("i");
    it.text = t;
    data_.lists[note_index_].items.push_back(it);
    save_local_lists();
  } else if (s == ScreenId::NotesDetail && note_index_ < static_cast<int>(data_.notes.size())) {
    data_.notes[note_index_].body += (data_.notes[note_index_].body.empty() ? "" : "\n") + t;
    data_.notes[note_index_].updated_at = now_ms_;
    if (data_.notes[note_index_].title == "Note" || data_.notes[note_index_].title.empty() ||
        data_.notes[note_index_].title == "Voice note") {
      data_.notes[note_index_].title = t.substr(0, 40);
    }
    save_local_notes();
  } else {
    Note n;
    n.id = new_local_id("n");
    n.title = t.substr(0, 40);
    n.body = t;
    n.created_at = n.updated_at = now_ms_;
    data_.notes.push_back(n);
    note_index_ = static_cast<int>(data_.notes.size()) - 1;
    save_local_notes();
    note_page_ = 0;
    nav_.replace(ScreenId::NotesDetail);
    after_nav();
  }
}

bool App::queue_offline_dictation(const std::vector<uint8_t>& pcm, bool open_note) {
  const ScreenId s = open_note ? nav_.current() : ScreenId::Lock;
  if (s == ScreenId::ListsDetail && note_index_ < static_cast<int>(data_.lists.size())) {
    auto& L = data_.lists[note_index_];
    // Online, a new clip re-records a failed one. Offline, every clip is a new item —
    // replacing would silently drop the earlier recording ("milk" then "eggs").
    for (auto& it : L.items) {
      if (it.pending_pcm.empty() || !wifi_.connected()) continue;
      const std::string old = it.pending_pcm;
      std::string name;
      if (!save_pending_pcm(pcm, &name)) return false;
      // Delete old only when the new file is distinct — same now_ms_/size would
      // overwrite then unlink the replacement.
      if (old != name) delete_pending_pcm(old);
      it.pending_pcm = name;
      it.text = kVoicePendingText;
      save_local_lists();
      error_msg_ = wifi_.connected() ? "Replaced clip — will retry transcription."
                                     : "Replaced clip — text when online.";
      error_until_ms_ = now_ms_ + 3500;
      mark_content_dirty();
      return true;
    }
    std::string name;
    if (!save_pending_pcm(pcm, &name)) return false;
    ListItem it;
    it.id = new_local_id("i");
    it.text = kVoicePendingText;
    it.pending_pcm = name;
    L.items.push_back(it);
    save_local_lists();
    error_msg_ = "Audio saved — it will transcribe when connected to Wi-Fi.";
    error_until_ms_ = now_ms_ + 3500;
    return true;
  }

  // Notes detail, or create a new note from list/elsewhere.
  if (s == ScreenId::NotesDetail && note_index_ >= 0 && note_index_ < static_cast<int>(data_.notes.size())) {
    auto& n = data_.notes[note_index_];
    if (!n.pending_pcm.empty() && !wifi_.connected()) {
      // Offline with a clip already waiting: keep both — the new clip becomes its own note.
      std::string name;
      if (!save_pending_pcm(pcm, &name)) return false;
      Note extra;
      extra.id = new_local_id("n");
      extra.title = n.title + " (2)";
      extra.body = kVoicePendingText;
      extra.pending_pcm = name;
      extra.created_at = extra.updated_at = now_ms_;
      data_.notes.push_back(extra);
      save_local_notes();
      error_msg_ = "Saved as a new note \xE2\x80\x94 text when online.";
      error_until_ms_ = now_ms_ + 3500;
      mark_content_dirty();
      return true;
    }
    if (!n.pending_pcm.empty()) {
      // Re-record replaces the waiting clip so online STT failures aren't a dead end.
      const std::string old = n.pending_pcm;
      std::string name;
      if (!save_pending_pcm(pcm, &name)) return false;
      if (old != name) delete_pending_pcm(old);
      n.pending_pcm = name;
      // Keep a single placeholder line — don't stack markers.
      if (n.body.find(kVoicePendingText) == std::string::npos &&
          n.body.find(kVoicePendingTextLegacy) == std::string::npos) {
        n.body += (n.body.empty() ? "" : "\n");
        n.body += kVoicePendingText;
      }
      n.updated_at = now_ms_;
      save_local_notes();
      error_msg_ = wifi_.connected() ? "Replaced clip — will retry transcription."
                                     : "Replaced clip — text when online.";
      error_until_ms_ = now_ms_ + 3500;
      mark_content_dirty();
      return true;
    }
    std::string name;
    if (!save_pending_pcm(pcm, &name)) return false;
    n.pending_pcm = name;
    n.body += (n.body.empty() ? "" : "\n");
    n.body += kVoicePendingText;
    n.updated_at = now_ms_;
    if (n.title == "Note" || n.title.empty()) n.title = "Voice note";
    save_local_notes();
    error_msg_ = "Audio saved — it will transcribe when connected to Wi-Fi.";
    error_until_ms_ = now_ms_ + 3500;
    return true;
  }

  std::string name;
  if (!save_pending_pcm(pcm, &name)) return false;
  Note n;
  n.id = new_local_id("n");
  n.title = "Voice note";
  n.body = kVoicePendingText;
  n.pending_pcm = name;
  n.created_at = n.updated_at = now_ms_;
  data_.notes.push_back(n);
  note_index_ = static_cast<int>(data_.notes.size()) - 1;
  save_local_notes();
  if (!open_note) return true;
  note_page_ = 0;
  nav_.replace(ScreenId::NotesDetail);
  after_nav();
  error_msg_ = "Audio saved — it will transcribe when connected to Wi-Fi.";
  error_until_ms_ = now_ms_ + 3500;
  return true;
}

void App::flush_pending_dictation() {
  if (!wifi_.connected() || !storage_) return;
  bool notes_changed = false;
  bool lists_changed = false;

  auto load_pcm = [&](const std::string& name, std::vector<uint8_t>& out) -> bool {
    if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos)
      return false;
    const std::string path = storage_->data_root() + "/notes/pcm/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
  };

  auto replace_pending_text = [](std::string& body, const std::string& transcript) {
    for (const char* m : {kVoicePendingText, kVoicePendingTextLegacy}) {
      const std::string marker = m;
      const size_t p = body.find(marker);
      if (p != std::string::npos) {
        body.replace(p, marker.size(), transcript);
        return;
      }
    }
    body += (body.empty() ? "" : "\n") + transcript;
  };

  for (auto& n : data_.notes) {
    if (n.pending_pcm.empty()) continue;
    std::vector<uint8_t> pcm;
    if (!load_pcm(n.pending_pcm, pcm)) {
      // Orphaned pointer — clear so the note isn't stuck forever.
      delete_pending_pcm(n.pending_pcm);
      n.pending_pcm.clear();
      replace_pending_text(n.body, "(Voice clip missing)");
      n.updated_at = now_ms_;
      notes_changed = true;
      continue;
    }
    std::string t = cloud_.stt_transcribe(pcm);
    if (t.size() == 1 && t[0] == '\x01') continue;  // transport — retry later
    if (t.empty()) {
      // Server answered but produced nothing. Give up after 3 tries so a bad clip
      // can't freeze the UI with a 90 s upload every minute forever.
      if (++stt_failures_[n.pending_pcm] >= 3) {
        stt_failures_.erase(n.pending_pcm);
        delete_pending_pcm(n.pending_pcm);
        n.pending_pcm.clear();
        replace_pending_text(n.body, "(Couldn't transcribe this voice clip)");
        n.updated_at = now_ms_;
        notes_changed = true;
      } else {
        error_msg_ = "Couldn't transcribe \xE2\x80\x94 hold to re-record.";
        error_until_ms_ = now_ms_ + 3500;
        mark_content_dirty();
      }
      continue;
    }
    const std::string done = n.pending_pcm;
    replace_pending_text(n.body, t);
    if (n.title == "Voice note" || n.title.empty() || n.title == "Note") n.title = t.substr(0, 40);
    n.pending_pcm.clear();
    n.updated_at = now_ms_;
    delete_pending_pcm(done);
    notes_changed = true;
  }

  for (auto& L : data_.lists) {
    for (auto& it : L.items) {
      if (it.pending_pcm.empty()) continue;
      std::vector<uint8_t> pcm;
      if (!load_pcm(it.pending_pcm, pcm)) {
        delete_pending_pcm(it.pending_pcm);
        it.pending_pcm.clear();
        it.text = "(Voice clip missing)";
        lists_changed = true;
        continue;
      }
      std::string t = cloud_.stt_transcribe(pcm);
      if (t.size() == 1 && t[0] == '\x01') continue;
      if (t.empty()) {
        if (++stt_failures_[it.pending_pcm] >= 3) {
          stt_failures_.erase(it.pending_pcm);
          delete_pending_pcm(it.pending_pcm);
          it.pending_pcm.clear();
          it.text = "(Couldn't transcribe this voice clip)";
          lists_changed = true;
        } else {
          error_msg_ = "Couldn't transcribe \xE2\x80\x94 hold to re-record.";
          error_until_ms_ = now_ms_ + 3500;
          mark_content_dirty();
        }
        continue;
      }
      const std::string done = it.pending_pcm;
      it.text = t;
      it.pending_pcm.clear();
      delete_pending_pcm(done);
      lists_changed = true;
    }
  }

  if (notes_changed) save_local_notes();
  if (lists_changed) save_local_lists();
  if (notes_changed || lists_changed) {
    error_msg_ = "Voice clip transcribed.";
    error_until_ms_ = now_ms_ + 2500;
    mark_content_dirty();
  }
}

bool App::weather_geocode_city() {
  if (cfg_.weather_city.empty()) return false;
  if (cfg_.weather_lat != 0.f || cfg_.weather_lon != 0.f) return true;
  std::string q;
  for (unsigned char c : cfg_.weather_city) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
        c == '-' || c == '\'') {
      if (c == ' ')
        q += "%20";
      else if (c == '\'')
        q += "%27";
      else
        q.push_back(static_cast<char>(c));
    }
  }
  if (q.empty()) return false;
  const std::string url =
      "https://geocoding-api.open-meteo.com/v1/search?name=" + q + "&count=1&language=en&format=json";
  const std::string body = cloud_.http_get_text(url);
  if (body.empty()) return false;
  // "latitude":47.6,"longitude":-122.3
  auto grab_num = [&](const char* key, float& dest) -> bool {
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return false;
    p = body.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
    dest = static_cast<float>(std::atof(body.c_str() + p));
    return true;
  };
  float lat = 0, lon = 0;
  if (!grab_num("latitude", lat) || !grab_num("longitude", lon)) return false;
  cfg_.weather_lat = lat;
  cfg_.weather_lon = lon;
  store_.save(cfg_);
  return true;
}

bool App::weather_fetch_forecast() {
  if (!wifi_.connected()) return false;
  if (!weather_geocode_city()) return false;
  char url[256];
  const char* unit = cfg_.weather_units ? "celsius" : "fahrenheit";
  std::snprintf(url, sizeof(url),
                "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                "&current=temperature_2m,weather_code"
                "&daily=weather_code,temperature_2m_max,temperature_2m_min"
                "&temperature_unit=%s&timezone=auto&forecast_days=5",
                static_cast<double>(cfg_.weather_lat), static_cast<double>(cfg_.weather_lon), unit);
  const std::string body = cloud_.http_get_text(url);
  JsonValue root;
  if (body.empty() || !json_parse(body, root) || !root.is_object()) return false;

  const JsonValue* cur = root.get("current");
  const JsonValue* daily = root.get("daily");
  if (!daily || !daily->is_object()) return false;
  const JsonValue* times = daily->get("time");
  const JsonValue* codes = daily->get("weather_code");
  const JsonValue* his = daily->get("temperature_2m_max");
  const JsonValue* los = daily->get("temperature_2m_min");
  if (!times || !codes || !his || !los || !times->is_array()) return false;

  std::vector<WeatherDay> days;
  for (size_t i = 0; i < times->items.size() && i < 5; ++i) {
    WeatherDay d;
    d.date = times->items[i].str;  // full YYYY-MM-DD — the weekday is derived from it
    if (i < codes->items.size()) d.condition = wmo_condition(static_cast<int>(codes->items[i].number));
    if (i < his->items.size()) d.hi = static_cast<int>(std::lround(his->items[i].number));
    if (i < los->items.size()) d.lo = static_cast<int>(std::lround(los->items[i].number));
    if (!d.date.empty()) days.push_back(std::move(d));
  }
  if (days.empty()) return false;

  data_.weather.days = std::move(days);
  data_.weather.city = cfg_.weather_city;
  data_.weather.units = cfg_.weather_units;
  if (cur && cur->is_object()) {
    data_.weather.today_temp = static_cast<int>(std::lround(cur->get_number("temperature_2m", data_.weather.today_temp)));
    data_.weather.today_condition = wmo_condition(static_cast<int>(cur->get_int("weather_code")));
  }
  if (data_.weather.today_condition.empty()) data_.weather.today_condition = data_.weather.days[0].condition;
  // Wall-clock seconds (survives restarts) so the screen can say when it was last updated.
  data_.weather.fetched_at = clock_.time_valid() ? static_cast<int64_t>(std::time(nullptr)) : 0;
  if (clock_.time_valid()) {
    int h = 0, m = 0, wd = 0, mo = 0, dd = 0;
    clock_.local_hm(h, m, wd, mo, dd);
    data_.weather.fetched_hm = h * 60 + m;
  }
  save_weather_cache();
  return true;
}

namespace {

struct City {
  const char* name;
  const char* tz;  // default for this IANA zone (nullptr = picker only)
  float lat, lon;
};
constexpr City kCities[] = {
    {"New York", "America/New_York", 40.7128f, -74.0060f},
    {"Boston", nullptr, 42.3601f, -71.0589f},
    {"Washington, DC", nullptr, 38.9072f, -77.0369f},
    {"Miami", nullptr, 25.7617f, -80.1918f},
    {"Chicago", "America/Chicago", 41.8781f, -87.6298f},
    {"Austin", nullptr, 30.2672f, -97.7431f},
    {"Denver", "America/Denver", 39.7392f, -104.9903f},
    {"Phoenix", "America/Phoenix", 33.4484f, -112.0740f},
    {"Los Angeles", "America/Los_Angeles", 34.0522f, -118.2437f},
    {"San Francisco", nullptr, 37.7749f, -122.4194f},
    {"Seattle", nullptr, 47.6062f, -122.3321f},
    {"Toronto", nullptr, 43.6532f, -79.3832f},
    {"London", "Europe/London", 51.5074f, -0.1278f},
    {"Paris", nullptr, 48.8566f, 2.3522f},
    {"Berlin", nullptr, 52.5200f, 13.4050f},
    {"Lisbon", nullptr, 38.7223f, -9.1393f},
    {"Tel Aviv", nullptr, 32.0853f, 34.7818f},
    {"Tokyo", nullptr, 35.6762f, 139.6503f},
    {"Sydney", nullptr, -33.8688f, 151.2093f},
    {"Reykjavík", "UTC", 64.1466f, -21.9426f},
};
constexpr int kCityCount = sizeof(kCities) / sizeof(kCities[0]);

}  // namespace

void App::weather_ensure_city() {
  if (!cfg_.weather_city.empty()) return;
  for (const auto& c : kCities) {
    if (c.tz && cfg_.tz_id == c.tz) {
      cfg_.weather_city = c.name;
      cfg_.weather_lat = c.lat;
      cfg_.weather_lon = c.lon;
      store_.save(cfg_);
      data_.weather = WeatherCache{};
      data_.weather.city = c.name;
      data_.weather.units = cfg_.weather_units;
      return;
    }
  }
}

int App::weather_display_temp(int v) const {
  if (data_.weather.units == cfg_.weather_units) return v;
  // Cache was fetched in the other unit — convert rather than mislabel ("72°C").
  if (cfg_.weather_units == 1) return static_cast<int>(std::lround((v - 32) * 5.0 / 9.0));
  return static_cast<int>(std::lround(v * 9.0 / 5.0 + 32));
}

void App::weather_refresh() {
  error_msg_.clear();
  if (!wifi_.connected()) {
    error_msg_ = "You're offline \xE2\x80\x94 showing the saved forecast.";
    error_until_ms_ = now_ms_ + 3500;
    transient_error_armed_ = true;
    mark_content_dirty();
    return;
  }
  if (cfg_.weather_city.empty()) {
    focus_.index = 0;
    nav_.push(ScreenId::WeatherCitySetup);
    after_nav();
    return;
  }
  show_status("Updating the forecast\xE2\x80\xA6");
  if (!weather_fetch_forecast()) {
    error_msg_ = "Couldn't refresh the forecast.";
    error_until_ms_ = now_ms_ + 3500;
    transient_error_armed_ = true;
  }
  mark_content_dirty();
}

void App::render_notes() {
  draw_status_bar();
  using ui::Icon;
  using ui::Row;
  using ui::RowKind;
  const ScreenId s = nav_.current();
  if (s == ScreenId::NotesList || s == ScreenId::ListsList) {
    static const char* kTabs[] = {"Notes", "Lists"};
    ui::tabs(canvas_, kContentTop, kTabs, 2, notes_tab_ == 0 ? 0 : 1);
  }
  const int list_top = kContentTop + kTabBand;

  if (s == ScreenId::NotesList) {
    const int n = static_cast<int>(data_.notes.size());
    if (n == 0) {
      ui::empty_state(canvas_, Icon::Notes, "No notes yet",
                      wifi_.connected() ? "Hold the back button to dictate, or choose New note."
                                        : "Works offline. Voice clips become text once you're online.",
                      360);
      focus_.count = 1;
    } else {
      focus_.count = n + 1;
      constexpr int kVisible = 8;
      const int start = ui::window_start(std::min(focus_.index, n - 1), n, kVisible);
      for (int k = 0; k < kVisible && start + k < n; ++k) {
        const int i = start + k;
        const auto& note = data_.notes[static_cast<size_t>(i)];
        Row r{note.title, note.pending_pcm.empty() ? RowKind::Nav : RowKind::Value,
              note.pending_pcm.empty() ? "" : "Voice"};
        ui::row(canvas_, row_y(list_top, k), r, i == focus_.index);
      }
      ui::scrollbar(canvas_, list_top, kVisible * kRowPitch - 8, start, kVisible, n);
    }
    ui::button(canvas_, kBottomCtaY, "New note", n == 0 || focus_.index == n, Icon::Plus, true);
  } else if (s == ScreenId::NotesDetail) {
    note_pages_ = 1;
    std::string title = "Note";
    if (note_index_ >= 0 && note_index_ < static_cast<int>(data_.notes.size()))
      title = data_.notes[static_cast<size_t>(note_index_)].title;
    ui::header(canvas_, title, true);
    if (note_index_ >= 0 && note_index_ < static_cast<int>(data_.notes.size())) {
      const auto& n = data_.notes[static_cast<size_t>(note_index_)];
      if (n.body.empty()) {
        canvas_.draw_text_wrapped(kSideMargin, kListTop, kContentW, kWrapGap,
                                  wifi_.connected() ? "Empty note. Hold the back button to dictate."
                                                    : "Empty note. Hold to capture a voice clip; text needs Wi-Fi.",
                                  Canvas::TextRole::Secondary, Gray::G0);
      } else {
        // Page the body so long notes never run under the actions.
        const auto lines = ui::wrap_text(canvas_, n.body, Canvas::TextRole::Body, kContentW);
        const int line_h = canvas_.text_height(Canvas::TextRole::Body) + 8;
        int limit = bottom_action_y(1) - 72;  // room for "Listening…" / pending caption
        int per_page = std::max(1, (limit - kListTop) / line_h);
        if (static_cast<int>(lines.size()) > per_page) {
          limit = bottom_action_y(2) - 60;
          per_page = std::max(1, (limit - kListTop) / line_h);
        }
        note_pages_ = std::max(1, (static_cast<int>(lines.size()) + per_page - 1) / per_page);
        note_page_ = std::clamp(note_page_, 0, note_pages_ - 1);
        int y = kListTop;
        for (int k = note_page_ * per_page; k < static_cast<int>(lines.size()) && k < (note_page_ + 1) * per_page; ++k) {
          canvas_.draw_text(kSideMargin, y, lines[static_cast<size_t>(k)], Canvas::TextRole::Body, Gray::G0);
          y += line_h;
        }
        if (!n.pending_pcm.empty()) {
          canvas_.draw_text_wrapped(kSideMargin, bottom_action_y(note_pages_ > 1 ? 2 : 1) - 64, kContentW, kWrapGap,
                                    wifi_.connected() ? "Still waiting for the transcript \xE2\x80\x94 hold to re-record."
                                                      : "Audio recording saved \xE2\x80\x94 it will transcribe on Wi-Fi.",
                                    Canvas::TextRole::Caption, Gray::G0);
        }
      }
    }
    if (ptt_active_) {
      const int ly = bottom_action_y(note_pages_ > 1 ? 2 : 1) - 40;
      ui::icon(canvas_, Icon::Mic, kCanvasW / 2 - 60, ly + 13, 22, Gray::G0);
      canvas_.draw_text(kCanvasW / 2 - 42, ly, "Listening\xE2\x80\xA6", Canvas::TextRole::BodyStrong, Gray::G0);
    }
    const int po = note_pages_ > 1 ? 1 : 0;
    if (po) {
      char pg[48];
      std::snprintf(pg, sizeof(pg), "%d of %d", note_page_ + 1, note_pages_);
      ui::row(canvas_, bottom_action_y(2),
              Row{note_page_ + 1 < note_pages_ ? "Next page" : "Back to start", RowKind::Value, pg},
              focus_.index == 0);
    }
    focus_.count = 2 + po;
    ui::row(canvas_, bottom_action_y(1), Row{"Dictate", RowKind::Action, {}, false, Icon::Mic, true},
            focus_.index == po);
    ui::row(canvas_, bottom_action_y(0), Row{"Delete note", RowKind::Plain, {}, false, Icon::Trash, true},
            focus_.index == po + 1);
  } else if (s == ScreenId::ListsList) {
    const int n = static_cast<int>(data_.lists.size());
    if (n == 0) {
      ui::empty_state(canvas_, Icon::Check, "No lists yet",
                      "Shopping, packing, to-dos. Lists stay on Pocket and sync when linked.", 360);
      focus_.count = 1;
    } else {
      focus_.count = n + 1;
      constexpr int kVisible = 8;
      const int start = ui::window_start(std::min(focus_.index, n - 1), n, kVisible);
      ui::scrollbar(canvas_, list_top, kVisible * kRowPitch - 8, start, kVisible, n);
      for (int k = 0; k < kVisible && start + k < n; ++k) {
        const int i = start + k;
        const auto& L = data_.lists[static_cast<size_t>(i)];
        int done = 0;
        for (const auto& it : L.items) done += it.checked ? 1 : 0;
        char cnt[24];
        std::snprintf(cnt, sizeof(cnt), "%d/%d", done, static_cast<int>(L.items.size()));
        ui::row(canvas_, row_y(list_top, k), Row{L.title, RowKind::Nav, cnt}, i == focus_.index);
      }
    }
    ui::button(canvas_, kBottomCtaY, "New list", n == 0 || focus_.index == n, Icon::Plus, true);
  } else if (s == ScreenId::ListsDetail) {
    if (note_index_ < static_cast<int>(data_.lists.size())) {
      auto& L = data_.lists[static_cast<size_t>(note_index_)];
      ui::header(canvas_, L.title, true);
      const int top = below_title(kContentTop);
      const int n = static_cast<int>(L.items.size());
      focus_.count = n + 1;
      constexpr int kVisible = 8;
      const int start = ui::window_start(std::min(focus_.index, std::max(0, n - 1)), n, kVisible);
      for (int k = 0; k < kVisible && start + k < n; ++k) {
        const size_t i = static_cast<size_t>(start + k);
        ui::row(canvas_, row_y(top, k), Row{L.items[i].text, RowKind::Check, {}, L.items[i].checked},
                static_cast<int>(i) == focus_.index);
      }
      ui::scrollbar(canvas_, top, kVisible * kRowPitch - 8, start, kVisible, n);
      if (L.items.empty()) {
        canvas_.draw_text_wrapped(kSideMargin, top, kContentW, kWrapGap,
                                  "Empty list. Hold the back button and say an item.", Canvas::TextRole::Secondary,
                                  Gray::G0);
      }
      ui::button(canvas_, kBottomCtaY, "Hold back button to add",
                 focus_.index == static_cast<int>(L.items.size()), Icon::Mic, true);
    }
  }

  if (now_ms_ < error_until_ms_ && !error_msg_.empty()) ui::toast(canvas_, error_msg_);
}

void App::handle_notes(InputEvent e) {
  ScreenId s = nav_.current();
  if (e == InputEvent::Back) {
    if (s == ScreenId::NotesDetail || s == ScreenId::ListsDetail) {
      nav_.pop();
      after_nav();
    } else {
      nav_.pop();
      after_nav();
    }
    return;
  }

  // Tab switch Notes ↔ Lists from either list root.
  if ((s == ScreenId::NotesList || s == ScreenId::ListsList) &&
      (e == InputEvent::Up || e == InputEvent::Down) && focus_.index == 0 && data_.notes.empty() &&
      data_.lists.empty() && e == InputEvent::Down) {
    // fall through to normal focus below when empty — still allow tab via Select on tab labels
  }
  if ((s == ScreenId::NotesList || s == ScreenId::ListsList) && e == InputEvent::Select &&
      focus_.count <= 1 && notes_tab_ == (s == ScreenId::NotesList ? 0 : 1)) {
    // Allow switching tabs: when on Notes tab with empty/fab, long-press Home is separate;
    // use Function while on "New note" doesn't switch. Explicit: Up from index 0 cycles tabs.
  }
  if ((s == ScreenId::NotesList || s == ScreenId::ListsList) && e == InputEvent::Up &&
      focus_.index == 0) {
    // Toggle tab when pressing Up on the first row / empty FAB.
    if (s == ScreenId::NotesList) {
      notes_tab_ = 1;
      focus_.index = 0;
      nav_.replace(ScreenId::ListsList);
      after_nav();
      return;
    }
    notes_tab_ = 0;
    focus_.index = 0;
    nav_.replace(ScreenId::NotesList);
    after_nav();
    return;
  }

  if (e == InputEvent::PttStart) {
    if (nav_.current() == ScreenId::ListsList) {
      // Voice adds items to an open list — not a stray Note from the overview.
      error_msg_ = "Open a list, then hold to add an item.";
      error_until_ms_ = now_ms_ + 2500;
      transient_error_armed_ = true;
      mark_content_dirty();
      return;
    }
    ptt_active_ = true;
    mic_capturing_ = true;
    if (audio_) audio_->start_capture();
    mark_content_dirty();
    return;
  }
  if (e == InputEvent::PttStop) {
    if (!ptt_active_) return;
    ptt_active_ = false;
    mic_capturing_ = false;
    const MicCaptureResult cap = audio_ ? audio_->stop_capture() : MicCaptureResult{};
    if (!cap.ok || cap.pcm.empty()) {
      error_msg_ = "Couldn't hear anything. Hold closer and try again.";
      error_until_ms_ = now_ms_ + 3000;
      mark_content_dirty();
      return;
    }
    // Cloud STT only (on-device STT not shipping). Offline → queue PCM + honest placeholder.
    if (!wifi_.connected()) {
      (void)queue_offline_dictation(cap.pcm);
      mark_content_dirty();
      return;
    }
    show_status("Transcribing\xE2\x80\xA6");
    std::string t = cloud_.stt_transcribe(cap.pcm);
    if (t.size() == 1 && t[0] == '\x01') {
      // Online but STT unreachable — still keep the clip so it isn't lost.
      if (queue_offline_dictation(cap.pcm)) {
        error_msg_ = "Speech service unreachable — clip saved for retry.";
        error_until_ms_ = now_ms_ + 3500;
      }
      mark_content_dirty();
      return;
    }
    if (t.empty()) {
      error_msg_ = "Couldn't hear anything. Hold closer and try again.";
      error_until_ms_ = now_ms_ + 3000;
      mark_content_dirty();
      return;
    }
    error_msg_.clear();
    apply_dictation_transcript(t);
    mark_content_dirty();
    return;
  }

  if (s == ScreenId::NotesList) {
    focus_.count = std::max(1, static_cast<int>(data_.notes.size()) + 1);
    if (e == InputEvent::Up || e == InputEvent::Down) {
      const int prev = focus_.index;
      focus_.move(e == InputEvent::Down ? 1 : -1);
      if (focus_.index == prev) return;
      const int n_notes = static_cast<int>(data_.notes.size());
      // List↔list: tight row region. Involving the FAB → content band (non-contiguous).
      if (prev < n_notes && focus_.index < n_notes) {
        mark_content_dirty();  // windowed list + bottom button: content band is always correct
      } else {
        mark_content_dirty();
      }
    } else if (e == InputEvent::Select) {
      if (focus_.index < static_cast<int>(data_.notes.size())) {
        note_index_ = focus_.index;
        note_page_ = 0;
        nav_.push(ScreenId::NotesDetail);
        after_nav();
      } else {
        Note n;
        n.id = new_local_id("n");
        n.title = "Note";
        n.body = "";
        n.created_at = n.updated_at = now_ms_;
        data_.notes.push_back(n);
        note_index_ = static_cast<int>(data_.notes.size()) - 1;
        save_local_notes();
        note_page_ = 0;
        nav_.push(ScreenId::NotesDetail);
        after_nav();
      }
    }
  } else if (s == ScreenId::NotesDetail) {
    const int po = note_pages_ > 1 ? 1 : 0;
    focus_.count = 2 + po;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      const int prev = focus_.index;
      focus_.move(e == InputEvent::Down ? 1 : -1);
      if (focus_.index == prev) return;
      mark_list_focus_dirty(bottom_action_y(1 + po), prev, focus_.index);
    } else if (e == InputEvent::Select && po && focus_.index == 0) {
      note_page_ = (note_page_ + 1) % note_pages_;
      mark_content_dirty();
    } else if (e == InputEvent::Select && focus_.index == po) {
      if (!wifi_.connected()) {
        error_msg_ = "Hold the back button to capture. Text needs Wi-Fi.";
      } else {
        error_msg_ = "Hold the back button to dictate.";
      }
      error_until_ms_ = now_ms_ + 3000;
      mark_content_dirty();
    } else if (e == InputEvent::Select && focus_.index == po + 1) {
      if (note_index_ < static_cast<int>(data_.notes.size())) {
        if (!data_.notes[note_index_].pending_pcm.empty()) {
          delete_pending_pcm(data_.notes[note_index_].pending_pcm);
        }
        data_.notes.erase(data_.notes.begin() + note_index_);
        save_local_notes();
        nav_.pop();
        after_nav();
      }
    }
  } else if (s == ScreenId::ListsList) {
    focus_.count = std::max(1, static_cast<int>(data_.lists.size()) + 1);
    if (e == InputEvent::Up || e == InputEvent::Down) {
      const int prev = focus_.index;
      focus_.move(e == InputEvent::Down ? 1 : -1);
      if (focus_.index == prev) return;
      const int n_lists = static_cast<int>(data_.lists.size());
      if (prev < n_lists && focus_.index < n_lists) {
        mark_content_dirty();  // windowed list + bottom button: content band is always correct
      } else {
        mark_content_dirty();
      }
    } else if (e == InputEvent::Select) {
      if (focus_.index < static_cast<int>(data_.lists.size())) {
        note_index_ = focus_.index;
        nav_.push(ScreenId::ListsDetail);
        after_nav();
      } else {
        TodoList L;
        L.id = new_local_id("L");
        L.title = "List";
        data_.lists.push_back(L);
        note_index_ = static_cast<int>(data_.lists.size()) - 1;
        save_local_lists();
        nav_.push(ScreenId::ListsDetail);
        after_nav();
      }
    }
  } else if (s == ScreenId::ListsDetail) {
    if (note_index_ < static_cast<int>(data_.lists.size())) {
      auto& L = data_.lists[note_index_];
      focus_.count = static_cast<int>(L.items.size()) + 1;
      if (e == InputEvent::Up || e == InputEvent::Down) {
        const int prev = focus_.index;
        focus_.move(e == InputEvent::Down ? 1 : -1);
        if (focus_.index == prev) return;
        const int n_items = static_cast<int>(L.items.size());
        if (prev < n_items && focus_.index < n_items) {
          mark_content_dirty();
        } else {
          mark_content_dirty();
        }
      } else if (e == InputEvent::Select && focus_.index < static_cast<int>(L.items.size())) {
        L.items[static_cast<size_t>(focus_.index)].checked =
            !L.items[static_cast<size_t>(focus_.index)].checked;
        save_local_lists();
        mark_content_dirty();
      }
    }
  }
}

void App::render_ledger() {
  draw_status_bar();
  ui::header(canvas_, "Ledger", true);
  ui::empty_state(canvas_, ui::Icon::Ledger, "Coming soon",
                  "Track who owes what. Ledger arrives in a free update.", 360);
  ui::button(canvas_, kBottomCtaY, "Back", true, ui::Icon::ChevronLeft, true);
}

void App::handle_ledger(InputEvent e) {
  if (e == InputEvent::Back || e == InputEvent::Select || e == InputEvent::Home) {
    go_home();
  }
}



void App::render_pass() {
  using ui::Icon;
  if (nav_.current() == ScreenId::PassDetail) {
    if (cfg_.parental_hide_pass_share) {
      draw_status_bar();
      ui::header(canvas_, "Pass", true);
      ui::empty_state(canvas_, Icon::Lock, "Pass hidden", "Parental controls hide pass sharing.", 360);
      return;
    }
    // Full-bleed scan view — no status bar, maximum quiet zone for scanners.
    const bool have = note_index_ >= 0 && note_index_ < static_cast<int>(data_.passes.size());
    const Pass* p = have ? &data_.passes[static_cast<size_t>(note_index_)] : nullptr;
    ui::icon(canvas_, Icon::ChevronLeft, kSideMargin + 9, 44, 24, Gray::G0);
    canvas_.draw_text_fit(kSideMargin + 28, 20, kContentW - 28, p ? p->title : std::string("Pass"),
                          Canvas::TextRole::ScreenTitle, Gray::G0);
    if (p && !p->type.empty()) {
      canvas_.draw_text_fit(kSideMargin + 28, 70, kContentW - 28, p->type, Canvas::TextRole::Caption, Gray::G0);
    }
    constexpr int kQr = 380;
    const int qr_x = (kCanvasW - kQr) / 2;
    const int qr_y = 140;
    if (!p || p->payload.empty() || !canvas_.draw_qr(qr_x, qr_y, kQr, p->payload)) {
      canvas_.stroke_round_rect(qr_x, qr_y, kQr, kQr, 16, Gray::G0, 2, Gray::G3);
      ui::empty_state(canvas_, Icon::Pass, "No code", "This pass has no scannable code.", qr_y + kQr / 2 + 30);
    }
    canvas_.draw_text_centered(kCanvasW / 2, qr_y + kQr + 40, "Hold steady for the scanner",
                               Canvas::TextRole::Caption, Gray::G0);
    return;
  }
  draw_status_bar();
  ui::header(canvas_, "Pass", true);
  if (cfg_.parental_hide_pass_share) {
    ui::empty_state(canvas_, Icon::Lock, "Pass sharing is off",
                    "Turned off in parental controls from Pocket Companion.", 360);
    return;
  }
  if (data_.passes.empty()) {
    ui::empty_state(canvas_, Icon::Pass, "No passes yet",
                    wifi_.connected() ? "Add boarding passes and tickets in Companion. They sync while online."
                                      : "Saved passes still open offline. Add new ones when online.",
                    360);
  } else {
    focus_.count = static_cast<int>(data_.passes.size());
    const int list_top = below_title(kContentTop);
    constexpr int kVisible = 10;
    const int start = ui::window_start(focus_.index, focus_.count, kVisible);
    for (int k = 0; k < kVisible && start + k < focus_.count; ++k) {
      const int i = start + k;
      const auto& p = data_.passes[static_cast<size_t>(i)];
      ui::row(canvas_, row_y(list_top, k), ui::Row{p.title, ui::RowKind::Nav, p.type, false, Icon::Pass, true},
              i == focus_.index);
    }
    ui::scrollbar(canvas_, list_top, kVisible * kRowPitch - 8, start, kVisible, focus_.count);
  }
}

void App::handle_pass(InputEvent e) {
  if (e == InputEvent::Back) {
    if (nav_.current() == ScreenId::PassDetail) {
      nav_.pop();
      after_nav();
    } else {
      nav_.pop();
      after_nav();
    }
    return;
  }
  if (cfg_.parental_hide_pass_share) return;
  if (nav_.current() == ScreenId::PassList && !data_.passes.empty()) {
    focus_.count = static_cast<int>(data_.passes.size());
    if (e == InputEvent::Up || e == InputEvent::Down) {
      const int prev = focus_.index;
      focus_.move(e == InputEvent::Down ? 1 : -1);
      if (focus_.index == prev) return;
      mark_content_dirty();
      return;
    }
    if (e == InputEvent::Select) {
      note_index_ = focus_.index;
      nav_.push(ScreenId::PassDetail);
      after_nav();
    }
  }
}

void App::render_weather() {
  draw_status_bar();
  using ui::Icon;
  using ui::Row;
  using ui::RowKind;

  if (nav_.current() == ScreenId::WeatherCitySetup) {
    ui::header(canvas_, "Weather city", true);
    focus_.count = kCityCount;
    constexpr int kVisible = 10;
    const int start = ui::window_start(focus_.index, kCityCount, kVisible);
    for (int k = 0; k < kVisible && start + k < kCityCount; ++k) {
      const int i = start + k;
      ui::row(canvas_, row_y(kListTop, k), Row{kCities[i].name, RowKind::Radio, {}, cfg_.weather_city == kCities[i].name},
              i == focus_.index);
    }
    ui::scrollbar(canvas_, kListTop, kVisible * kRowPitch - 8, start, kVisible, kCityCount);
    return;
  }

  if (data_.weather.city.empty() && !cfg_.weather_city.empty()) data_.weather.city = cfg_.weather_city;
  focus_.count = 2;
  if (data_.weather.city.empty()) {
    ui::header(canvas_, "Weather", true);
    ui::empty_state(canvas_, Icon::Weather, "Choose your city", "Pick a city for the forecast.", 360);
    focus_.count = 1;
    ui::button(canvas_, kBottomCtaY, "Choose city", true, Icon::ChevronRight, true);
    return;
  }
  ui::header(canvas_, data_.weather.city, true);

  if (data_.weather.days.empty()) {
    ui::empty_state(canvas_, Icon::Cloud, wifi_.connected() ? "Getting the forecast\xE2\x80\xA6" : "No forecast yet",
                    wifi_.connected() ? "This takes a moment." : "Connect to Wi-Fi to load the forecast.", 360);
  } else {
    {
      std::string cond = data_.weather.today_condition;
      for (auto& ch : cond) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      Icon wi = Icon::Weather;
      if (cond.find("rain") != std::string::npos || cond.find("drizzle") != std::string::npos ||
          cond.find("shower") != std::string::npos)
        wi = Icon::Rain;
      else if (cond.find("cloud") != std::string::npos || cond.find("overcast") != std::string::npos ||
               cond.find("fog") != std::string::npos)
        wi = Icon::Cloud;
      ui::icon(canvas_, wi, kCanvasW - kSideMargin - 48, below_title(kContentTop) + 44, 88, Gray::G0);
    }
    char tbuf[32];
    std::snprintf(tbuf, sizeof(tbuf), "%d\xC2\xB0", weather_display_temp(data_.weather.today_temp));
    const int temp_y = below_title(kContentTop) - 8;
    canvas_.draw_text(kSideMargin - 4, temp_y, tbuf, Canvas::TextRole::Hero, Gray::G0);
    const int tw = canvas_.text_width(tbuf, Canvas::TextRole::Hero);
    canvas_.draw_text(kSideMargin + tw + 2, temp_y + 22, cfg_.weather_units ? "C" : "F", Canvas::TextRole::BodyStrong,
                      Gray::G0);
    const int cond_y = temp_y + canvas_.text_height(Canvas::TextRole::Hero) + 4;
    canvas_.draw_text_fit(kSideMargin, cond_y, kContentW, data_.weather.today_condition, Canvas::TextRole::Body,
                          Gray::G0);
    static const char* kDow[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    auto weekday_of = [](const std::string& iso) -> int {
      int y = 0, m = 0, d = 0;
      if (std::sscanf(iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) return -1;
      static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
      if (m < 3) y -= 1;
      return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    };
    const int forecast_top = cond_y + kBodyLinePitch + kSectionGap + 8;
    constexpr int kRowH = 52;
    for (size_t i = 0; i < data_.weather.days.size() && i < 5; ++i) {
      const auto& d = data_.weather.days[i];
      const int ry = forecast_top + static_cast<int>(i) * kRowH;
      canvas_.dotted_hline(kSideMargin, ry - 10, kContentW, Gray::G0, 3);
      const int wd = weekday_of(d.date);
      const char* day = i == 0 ? "Today" : (wd >= 0 ? kDow[wd] : d.date.c_str());
      canvas_.draw_text(kSideMargin, ry, day, Canvas::TextRole::BodyStrong, Gray::G0);
      char hl[32];
      std::snprintf(hl, sizeof(hl), "%d\xC2\xB0 / %d\xC2\xB0", weather_display_temp(d.hi), weather_display_temp(d.lo));
      canvas_.draw_text(kSideMargin + 108, ry + 3, hl, Canvas::TextRole::Secondary, Gray::G0);
      canvas_.draw_text_fit(kSideMargin + 232, ry + 3, kContentW - 232, d.condition, Canvas::TextRole::Secondary,
                            Gray::G0);
    }
  }

  // Status line: always say how fresh the forecast is ("Last updated yesterday at 6:10 PM").
  std::string upd = weather_updated_label();
  if (!wifi_.connected() && !data_.weather.days.empty()) upd = "Offline \xC2\xB7 " + upd;
  if (!upd.empty())
    canvas_.draw_text_fit(kSideMargin, kBottomCtaY - kRowPitch - 34, kContentW, upd, Canvas::TextRole::Caption,
                          Gray::G0);
  ui::row(canvas_, kBottomCtaY - kRowPitch, Row{"Refresh", RowKind::Plain, {}, false, Icon::Update, true},
          focus_.index == 0);
  ui::row(canvas_, kBottomCtaY, Row{"Change city", RowKind::Nav, {}, false, Icon::Weather, true}, focus_.index == 1);
  if (now_ms_ < error_until_ms_ && !error_msg_.empty()) ui::toast(canvas_, error_msg_);
}

std::string App::weather_updated_label() const {
  if (data_.weather.days.empty()) return {};
  auto clock_str = [&](int h, int m) {
    char b[24];
    if (cfg_.time_format == 24) std::snprintf(b, sizeof(b), "%02d:%02d", h, m);
    else std::snprintf(b, sizeof(b), "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
    return std::string(b);
  };
  const int64_t at = data_.weather.fetched_at;
  if (at > 1000000000LL && clock_.time_valid()) {  // wall-clock seconds
    const time_t then_t = static_cast<time_t>(at), now_t = std::time(nullptr);
    struct tm then {}, now {};
    localtime_r(&then_t, &then);
    localtime_r(&now_t, &now);
    const std::string hm = clock_str(then.tm_hour, then.tm_min);
    // Whole days between the two local dates.
    struct tm a = then, b = now;
    a.tm_hour = b.tm_hour = 12;
    a.tm_min = b.tm_min = a.tm_sec = b.tm_sec = 0;
    const long days = std::lround(std::difftime(std::mktime(&b), std::mktime(&a)) / 86400.0);
    static const char* kDays[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (days <= 0) return "Last updated today at " + hm;
    if (days == 1) return "Last updated yesterday at " + hm;
    if (days < 7) return std::string("Last updated ") + kDays[then.tm_wday] + " at " + hm;
    return std::string("Last updated ") + kMon[then.tm_mon] + " " + std::to_string(then.tm_mday);
  }
  // Older cache (no wall time stored): best we know is the time of day.
  if (data_.weather.fetched_hm >= 0)
    return "Last updated at " + clock_str(data_.weather.fetched_hm / 60, data_.weather.fetched_hm % 60);
  return "Saved forecast";
}

void App::handle_weather(InputEvent e) {
  const bool rot = e == InputEvent::Up || e == InputEvent::Down;
  if (nav_.current() == ScreenId::WeatherCitySetup) {
    focus_.count = kCityCount;
    if (e == InputEvent::Back) {
      nav_.pop();
      after_nav();
    } else if (rot) {
      const int prev = focus_.index;
      focus_.move(e == InputEvent::Down ? 1 : -1);
      if (focus_.index != prev) mark_content_dirty();
    } else if (e == InputEvent::Select && focus_.index >= 0 && focus_.index < kCityCount) {
      const City& c = kCities[focus_.index];
      cfg_.weather_city = c.name;
      cfg_.weather_lat = c.lat;
      cfg_.weather_lon = c.lon;
      store_.save(cfg_);
      data_.weather = WeatherCache{};
      data_.weather.city = c.name;
      data_.weather.units = cfg_.weather_units;
      save_weather_cache();
      weather_refresh_pending_ = true;
      focus_.index = 0;
      nav_.pop();
      after_nav();
    }
    return;
  }
  if (e == InputEvent::Back) {
    nav_.pop();
    after_nav();
    return;
  }
  if (cfg_.weather_city.empty() && data_.weather.city.empty()) {
    if (e == InputEvent::Select) {
      focus_.index = 0;
      nav_.push(ScreenId::WeatherCitySetup);
      after_nav();
    }
    return;
  }
  focus_.count = 2;
  if (rot) {
    const int prev = focus_.index;
    focus_.move(e == InputEvent::Down ? 1 : -1);
    if (focus_.index != prev) mark_region_dirty(0, kBottomCtaY - kRowPitch - 4, kCanvasW, kRowPitch + kFocusRowH + 8);
    return;
  }
  if (e == InputEvent::Select) {
    if (focus_.index == 1) {
      // Open the picker at the current city.
      focus_.index = 0;
      for (int i = 0; i < kCityCount; ++i)
        if (cfg_.weather_city == kCities[i].name) focus_.index = i;
      nav_.push(ScreenId::WeatherCitySetup);
      after_nav();
    } else {
      weather_refresh();
    }
  }
}

}  // namespace pocket
