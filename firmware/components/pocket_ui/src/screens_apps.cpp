#include "pocket/app.hpp"
#include "pocket/ui.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace pocket {
namespace {

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o.push_back('\\');
      o.push_back(static_cast<char>(c));
    } else if (c == '\n') {
      o += "\\n";
    } else if (c == '\r') {
      o += "\\r";
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      o += buf;
    } else {
      o.push_back(static_cast<char>(c));
    }
  }
  return o;
}

std::string json_unescape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char n = s[++i];
      if (n == 'n')
        o.push_back('\n');
      else if (n == 'r')
        o.push_back('\r');
      else if (n == 't')
        o.push_back('\t');
      else if (n == 'u' && i + 4 < s.size()) {
        // Skip simple \u00XX ascii escapes.
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
          const char h = s[++i];
          v <<= 4;
          if (h >= '0' && h <= '9')
            v |= static_cast<unsigned>(h - '0');
          else if (h >= 'a' && h <= 'f')
            v |= static_cast<unsigned>(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F')
            v |= static_cast<unsigned>(h - 'A' + 10);
        }
        if (v < 128) o.push_back(static_cast<char>(v));
      } else {
        o.push_back(n);
      }
    } else {
      o.push_back(s[i]);
    }
  }
  return o;
}

bool grab_json_string(const std::string& obj, const char* key, std::string& dest) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t k = obj.find(needle);
  if (k == std::string::npos) return false;
  size_t colon = obj.find(':', k + needle.size());
  if (colon == std::string::npos) return false;
  size_t q1 = obj.find('"', colon + 1);
  if (q1 == std::string::npos) return false;
  std::string raw;
  for (size_t i = q1 + 1; i < obj.size(); ++i) {
    if (obj[i] == '\\' && i + 1 < obj.size()) {
      raw.push_back(obj[i]);
      raw.push_back(obj[++i]);
      continue;
    }
    if (obj[i] == '"') {
      dest = json_unescape(raw);
      return true;
    }
    raw.push_back(obj[i]);
  }
  return false;
}

bool grab_json_int64(const std::string& obj, const char* key, int64_t& dest) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t k = obj.find(needle);
  if (k == std::string::npos) return false;
  size_t colon = obj.find(':', k + needle.size());
  if (colon == std::string::npos) return false;
  size_t i = colon + 1;
  while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t')) ++i;
  dest = static_cast<int64_t>(std::atoll(obj.c_str() + i));
  return true;
}

}  // namespace

void App::load_local_notes() {
  data_.notes.clear();
  if (!storage_ || !storage_->data_ensure_root()) return;
  const std::string path = storage_->data_root() + "/notes/index.json";
  std::ifstream in(path);
  if (!in) return;
  std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t i = 0;
  while (i < json.size()) {
    const size_t obj = json.find('{', i);
    if (obj == std::string::npos) break;
    const size_t end = json.find('}', obj + 1);
    if (end == std::string::npos) break;
    const std::string obj_s = json.substr(obj, end - obj + 1);
    Note n;
    grab_json_string(obj_s, "id", n.id);
    grab_json_string(obj_s, "title", n.title);
    grab_json_string(obj_s, "body", n.body);
    grab_json_int64(obj_s, "created_at", n.created_at);
    grab_json_int64(obj_s, "updated_at", n.updated_at);
    grab_json_string(obj_s, "pending", n.pending_pcm);
    if (!n.id.empty()) data_.notes.push_back(n);
    i = end + 1;
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
  const std::string path = dir + "/index.json";
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    error_msg_ = "Couldn't write notes to local storage.";
    error_until_ms_ = now_ms_ + 3000;
    return false;
  }
  out << "[";
  for (size_t i = 0; i < data_.notes.size(); ++i) {
    const auto& n = data_.notes[i];
    if (i) out << ",";
    out << "{\"id\":\"" << json_escape(n.id) << "\",\"title\":\"" << json_escape(n.title)
        << "\",\"body\":\"" << json_escape(n.body) << "\",\"created_at\":" << n.created_at
        << ",\"updated_at\":" << n.updated_at;
    if (!n.pending_pcm.empty())
      out << ",\"pending\":\"" << json_escape(n.pending_pcm) << "\"";
    out << "}";
  }
  out << "]";
  return static_cast<bool>(out);
}

void App::load_local_lists() {
  data_.lists.clear();
  if (!storage_ || !storage_->data_ensure_root()) return;
  const std::string path = storage_->data_root() + "/lists/index.json";
  std::ifstream in(path);
  if (!in) return;
  std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  // Minimal: [{id,title,items:[{text,checked},...]}]
  size_t i = 0;
  while (i < json.size()) {
    const size_t obj = json.find("{\"id\"", i);
    if (obj == std::string::npos) {
      const size_t obj2 = json.find('{', i);
      if (obj2 == std::string::npos) break;
      // fall through with obj2
    }
    const size_t start = (obj == std::string::npos) ? json.find('{', i) : obj;
    if (start == std::string::npos) break;
    // Find matching end by scanning for items array end — tolerant parser.
    size_t end = start + 1;
    int depth = 1;
    while (end < json.size() && depth > 0) {
      if (json[end] == '{') ++depth;
      else if (json[end] == '}') --depth;
      ++end;
    }
    const std::string obj_s = json.substr(start, end - start);
    TodoList L;
    grab_json_string(obj_s, "id", L.id);
    grab_json_string(obj_s, "title", L.title);
    size_t items = obj_s.find("\"items\"");
    if (items != std::string::npos) {
      size_t arr = obj_s.find('[', items);
      size_t arr_end = obj_s.rfind(']');
      if (arr != std::string::npos && arr_end != std::string::npos && arr_end > arr) {
        size_t p = arr + 1;
        while (p < arr_end) {
          size_t io = obj_s.find('{', p);
          if (io == std::string::npos || io >= arr_end) break;
          size_t ie = obj_s.find('}', io + 1);
          if (ie == std::string::npos || ie > arr_end) break;
          const std::string item = obj_s.substr(io, ie - io + 1);
          ListItem it;
          grab_json_string(item, "text", it.text);
          grab_json_string(item, "pending", it.pending_pcm);
          if (item.find("\"checked\":true") != std::string::npos ||
              item.find("\"checked\": true") != std::string::npos)
            it.checked = true;
          if (!it.text.empty() || !it.pending_pcm.empty()) L.items.push_back(it);
          p = ie + 1;
        }
      }
    }
    if (!L.id.empty()) data_.lists.push_back(L);
    i = end;
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
  const std::string path = dir + "/index.json";
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    error_msg_ = "Couldn't write lists to local storage.";
    error_until_ms_ = now_ms_ + 3000;
    return false;
  }
  out << "[";
  for (size_t i = 0; i < data_.lists.size(); ++i) {
    const auto& L = data_.lists[i];
    if (i) out << ",";
    out << "{\"id\":\"" << json_escape(L.id) << "\",\"title\":\"" << json_escape(L.title)
        << "\",\"items\":[";
    for (size_t j = 0; j < L.items.size(); ++j) {
      if (j) out << ",";
      out << "{\"text\":\"" << json_escape(L.items[j].text)
          << "\",\"checked\":" << (L.items[j].checked ? "true" : "false");
      if (!L.items[j].pending_pcm.empty())
        out << ",\"pending\":\"" << json_escape(L.items[j].pending_pcm) << "\"";
      out << "}";
    }
    out << "]}";
  }
  out << "]";
  return static_cast<bool>(out);
}

void App::load_local_passes() {
  data_.passes.clear();
  if (!storage_ || !storage_->data_ensure_root()) return;
  const std::string path = storage_->data_root() + "/passes/index.json";
  std::ifstream in(path);
  if (!in) return;
  std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t i = 0;
  while (i < json.size()) {
    const size_t obj = json.find('{', i);
    if (obj == std::string::npos) break;
    const size_t end = json.find('}', obj + 1);
    if (end == std::string::npos) break;
    const std::string obj_s = json.substr(obj, end - obj + 1);
    Pass p;
    grab_json_string(obj_s, "id", p.id);
    grab_json_string(obj_s, "title", p.title);
    grab_json_string(obj_s, "type", p.type);
    grab_json_string(obj_s, "payload", p.payload);
    if (!p.id.empty()) {
      if (p.title.empty()) p.title = p.id;
      data_.passes.push_back(std::move(p));
    }
    i = end + 1;
  }
}

void App::save_local_passes() {
  if (!storage_ || !storage_->data_ensure_root()) return;
  const std::string dir = storage_->data_root() + "/passes";
  ::mkdir(dir.c_str(), 0755);
  const std::string path = dir + "/index.json";
  std::ofstream out(path, std::ios::trunc);
  if (!out) return;
  out << "[";
  for (size_t i = 0; i < data_.passes.size(); ++i) {
    const auto& p = data_.passes[i];
    if (i) out << ",";
    out << "{\"id\":\"" << json_escape(p.id) << "\",\"title\":\"" << json_escape(p.title)
        << "\",\"type\":\"" << json_escape(p.type) << "\",\"payload\":\"" << json_escape(p.payload)
        << "\"}";
  }
  out << "]";
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
  const std::string path = storage_->data_root() + "/weather/cache.json";
  std::ifstream in(path);
  if (!in) {
    if (!cfg_.weather_city.empty()) data_.weather.city = cfg_.weather_city;
    data_.weather.units = cfg_.weather_units;
    return;
  }
  std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  grab_json_string(json, "city", data_.weather.city);
  int64_t fetched = 0;
  grab_json_int64(json, "fetched_at", fetched);
  data_.weather.fetched_at = fetched;
  int64_t units = 0;
  if (grab_json_int64(json, "units", units)) data_.weather.units = static_cast<uint8_t>(units);
  int64_t temp = 0;
  if (grab_json_int64(json, "today_temp", temp)) data_.weather.today_temp = static_cast<int>(temp);
  grab_json_string(json, "today_condition", data_.weather.today_condition);
  data_.weather.days.clear();
  size_t days = json.find("\"days\"");
  if (days != std::string::npos) {
    size_t i = days;
    while (i < json.size()) {
      const size_t obj = json.find('{', i);
      if (obj == std::string::npos) break;
      const size_t end = json.find('}', obj + 1);
      if (end == std::string::npos) break;
      const std::string obj_s = json.substr(obj, end - obj + 1);
      WeatherDay d;
      grab_json_string(obj_s, "date", d.date);
      int64_t hi = 0, lo = 0;
      grab_json_int64(obj_s, "hi", hi);
      grab_json_int64(obj_s, "lo", lo);
      d.hi = static_cast<int>(hi);
      d.lo = static_cast<int>(lo);
      grab_json_string(obj_s, "condition", d.condition);
      if (!d.date.empty()) data_.weather.days.push_back(std::move(d));
      i = end + 1;
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
  const std::string path = dir + "/cache.json";
  std::ofstream out(path, std::ios::trunc);
  if (!out) return;
  out << "{\"city\":\"" << json_escape(data_.weather.city) << "\",\"units\":" << static_cast<int>(data_.weather.units)
      << ",\"fetched_at\":" << data_.weather.fetched_at << ",\"today_temp\":" << data_.weather.today_temp
      << ",\"today_condition\":\"" << json_escape(data_.weather.today_condition) << "\",\"days\":[";
  for (size_t i = 0; i < data_.weather.days.size(); ++i) {
    const auto& d = data_.weather.days[i];
    if (i) out << ",";
    out << "{\"date\":\"" << json_escape(d.date) << "\",\"hi\":" << d.hi << ",\"lo\":" << d.lo
        << ",\"condition\":\"" << json_escape(d.condition) << "\"}";
  }
  out << "]}";
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
    n.id = std::to_string(now_ms_) + "-" + std::to_string(data_.notes.size() + 1);
    n.title = t.substr(0, 40);
    n.body = t;
    n.created_at = n.updated_at = now_ms_;
    data_.notes.push_back(n);
    note_index_ = static_cast<int>(data_.notes.size()) - 1;
    save_local_notes();
    nav_.replace(ScreenId::NotesDetail);
    after_nav();
  }
}

bool App::queue_offline_dictation(const std::vector<uint8_t>& pcm) {
  const ScreenId s = nav_.current();
  if (s == ScreenId::ListsDetail && note_index_ < static_cast<int>(data_.lists.size())) {
    auto& L = data_.lists[note_index_];
    // Re-record replaces any existing pending clip on this list (recover from failed STT).
    for (auto& it : L.items) {
      if (it.pending_pcm.empty()) continue;
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
    it.text = kVoicePendingText;
    it.pending_pcm = name;
    L.items.push_back(it);
    save_local_lists();
    error_msg_ = "Saved — will become text when online.";
    error_until_ms_ = now_ms_ + 3500;
    return true;
  }

  // Notes detail, or create a new note from list/elsewhere.
  if (s == ScreenId::NotesDetail && note_index_ >= 0 && note_index_ < static_cast<int>(data_.notes.size())) {
    auto& n = data_.notes[note_index_];
    if (!n.pending_pcm.empty()) {
      // Re-record replaces the waiting clip so online STT failures aren't a dead end.
      const std::string old = n.pending_pcm;
      std::string name;
      if (!save_pending_pcm(pcm, &name)) return false;
      if (old != name) delete_pending_pcm(old);
      n.pending_pcm = name;
      // Keep a single placeholder line — don't stack markers.
      if (n.body.find(kVoicePendingText) == std::string::npos) {
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
    error_msg_ = "Saved — will become text when online.";
    error_until_ms_ = now_ms_ + 3500;
    return true;
  }

  std::string name;
  if (!save_pending_pcm(pcm, &name)) return false;
  Note n;
  n.id = std::to_string(now_ms_) + "-" + std::to_string(data_.notes.size() + 1);
  n.title = "Voice note";
  n.body = kVoicePendingText;
  n.pending_pcm = name;
  n.created_at = n.updated_at = now_ms_;
  data_.notes.push_back(n);
  note_index_ = static_cast<int>(data_.notes.size()) - 1;
  save_local_notes();
  nav_.replace(ScreenId::NotesDetail);
  after_nav();
  error_msg_ = "Saved — will become text when online.";
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
    const std::string marker = kVoicePendingText;
    const size_t p = body.find(marker);
    if (p != std::string::npos) {
      body.replace(p, marker.size(), transcript);
      // Drop a dangling newline left if transcript empty (shouldn't happen).
      return;
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
      // Keep clip; surface failure so online + pending isn't silent.
      error_msg_ = "Couldn't transcribe — hold to re-record.";
      error_until_ms_ = now_ms_ + 3500;
      mark_content_dirty();
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
        error_msg_ = "Couldn't transcribe — hold to re-record.";
        error_until_ms_ = now_ms_ + 3500;
        mark_content_dirty();
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
  if (body.empty()) return false;

  auto find_arr = [&](const char* key) -> size_t {
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return std::string::npos;
    return body.find('[', p);
  };
  auto next_num = [&](size_t& i) -> double {
    while (i < body.size() &&
           (body[i] == ' ' || body[i] == '\t' || body[i] == ',' || body[i] == '[' || body[i] == '\n'))
      ++i;
    const char* s = body.c_str() + i;
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (end) i = static_cast<size_t>(end - body.c_str());
    return v;
  };
  auto next_str = [&](size_t& i) -> std::string {
    while (i < body.size() && body[i] != '"') ++i;
    if (i >= body.size()) return {};
    ++i;
    size_t end = body.find('"', i);
    if (end == std::string::npos) return {};
    std::string s = body.substr(i, end - i);
    i = end + 1;
    return s;
  };

  // current temperature + weather_code (first occurrence after "current")
  size_t cur = body.find("\"current\"");
  int cur_temp = data_.weather.today_temp;
  int cur_code = 0;
  if (cur != std::string::npos) {
    size_t t = body.find("\"temperature_2m\"", cur);
    if (t != std::string::npos) {
      t = body.find(':', t);
      if (t != std::string::npos) cur_temp = static_cast<int>(std::lround(std::atof(body.c_str() + t + 1)));
    }
    size_t c = body.find("\"weather_code\"", cur);
    if (c != std::string::npos && c < body.find("\"daily\"", cur)) {
      c = body.find(':', c);
      if (c != std::string::npos) cur_code = static_cast<int>(std::atoi(body.c_str() + c + 1));
    }
  }

  size_t times = find_arr("time");
  size_t codes = find_arr("weather_code");
  // Prefer daily weather_code array (second weather_code after daily)
  size_t daily = body.find("\"daily\"");
  if (daily != std::string::npos) {
    size_t dt = body.find("\"time\"", daily);
    size_t dc = body.find("\"weather_code\"", daily);
    size_t dhi = body.find("\"temperature_2m_max\"", daily);
    size_t dlo = body.find("\"temperature_2m_min\"", daily);
    if (dt != std::string::npos) times = body.find('[', dt);
    if (dc != std::string::npos) codes = body.find('[', dc);
    size_t his = (dhi != std::string::npos) ? body.find('[', dhi) : std::string::npos;
    size_t los = (dlo != std::string::npos) ? body.find('[', dlo) : std::string::npos;
    if (times == std::string::npos || codes == std::string::npos || his == std::string::npos ||
        los == std::string::npos)
      return false;
    size_t ti = times + 1, ci = codes + 1, hi = his + 1, lo = los + 1;
    data_.weather.days.clear();
    for (int n = 0; n < 5; ++n) {
      WeatherDay d;
      d.date = next_str(ti);
      d.condition = wmo_condition(static_cast<int>(next_num(ci)));
      d.hi = static_cast<int>(std::lround(next_num(hi)));
      d.lo = static_cast<int>(std::lround(next_num(lo)));
      if (d.date.size() >= 10) d.date = d.date.substr(5);  // MM-DD
      if (d.date.empty()) break;
      data_.weather.days.push_back(std::move(d));
    }
  } else {
    return false;
  }

  data_.weather.city = cfg_.weather_city.empty() ? data_.weather.city : cfg_.weather_city;
  data_.weather.units = cfg_.weather_units;
  data_.weather.today_temp = cur_temp;
  data_.weather.today_condition = wmo_condition(cur_code);
  if (data_.weather.today_condition.empty() && !data_.weather.days.empty())
    data_.weather.today_condition = data_.weather.days[0].condition;
  data_.weather.fetched_at = static_cast<int64_t>(now_ms_);
  save_weather_cache();
  return true;
}

void App::weather_refresh() {
  error_msg_.clear();
  if (!wifi_.connected()) {
    error_msg_ = "You're offline. Showing saved forecast.";
    error_until_ms_ = now_ms_ + 3500;
    mark_content_dirty();
    return;
  }
  if (cfg_.weather_city.empty()) {
    error_msg_ = "Set a city in Settings → Units / Weather.";
    error_until_ms_ = now_ms_ + 3500;
    mark_content_dirty();
    return;
  }
  if (!weather_fetch_forecast()) {
    error_msg_ = "Couldn't refresh weather.";
    error_until_ms_ = now_ms_ + 3500;
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
                      wifi_.connected() ? "Hold the side button to dictate, or choose New note."
                                        : "Works offline. Voice clips become text once you're online.",
                      360);
      focus_.count = 1;
    } else {
      focus_.count = n + 1;
      for (int i = 0; i < n && row_y(list_top, i) + kFocusRowH < kBottomCtaY - 8; ++i) {
        const auto& note = data_.notes[static_cast<size_t>(i)];
        Row r{note.title, note.pending_pcm.empty() ? RowKind::Nav : RowKind::Value,
              note.pending_pcm.empty() ? "" : "Voice"};
        ui::row(canvas_, row_y(list_top, i), r, i == focus_.index);
      }
    }
    ui::button(canvas_, kBottomCtaY, "New note", n == 0 || focus_.index == n, Icon::Plus, true);
  } else if (s == ScreenId::NotesDetail) {
    std::string title = "Note";
    if (note_index_ >= 0 && note_index_ < static_cast<int>(data_.notes.size()))
      title = data_.notes[static_cast<size_t>(note_index_)].title;
    ui::header(canvas_, title, true);
    if (note_index_ >= 0 && note_index_ < static_cast<int>(data_.notes.size())) {
      const auto& n = data_.notes[static_cast<size_t>(note_index_)];
      if (n.body.empty()) {
        canvas_.draw_text_wrapped(kSideMargin, kListTop, kContentW, kWrapGap,
                                  wifi_.connected() ? "Empty note. Hold the side button to dictate."
                                                    : "Empty note. Hold to capture a voice clip; text needs Wi-Fi.",
                                  Canvas::TextRole::Secondary, Gray::G0);
      } else {
        canvas_.draw_text_wrapped(kSideMargin, kListTop, kContentW, 8, n.body, Canvas::TextRole::Body, Gray::G0);
        if (!n.pending_pcm.empty()) {
          canvas_.draw_text_wrapped(kSideMargin, bottom_action_y(1) - 64, kContentW, kWrapGap,
                                    wifi_.connected() ? "Still waiting for the transcript \xE2\x80\x94 hold to re-record."
                                                      : "Voice clip saved \xE2\x80\x94 text arrives when online.",
                                    Canvas::TextRole::Caption, Gray::G0);
        }
      }
    }
    if (ptt_active_) {
      const int ly = bottom_action_y(1) - 40;
      ui::icon(canvas_, Icon::Mic, kCanvasW / 2 - 60, ly + 13, 22, Gray::G0);
      canvas_.draw_text(kCanvasW / 2 - 42, ly, "Listening\xE2\x80\xA6", Canvas::TextRole::BodyStrong, Gray::G0);
    }
    focus_.count = 2;
    ui::row(canvas_, bottom_action_y(1), Row{"Dictate", RowKind::Action, {}, false, Icon::Mic, true},
            focus_.index == 0);
    ui::row(canvas_, bottom_action_y(0), Row{"Delete note", RowKind::Plain, {}, false, Icon::Trash, true},
            focus_.index == 1);
  } else if (s == ScreenId::ListsList) {
    const int n = static_cast<int>(data_.lists.size());
    if (n == 0) {
      ui::empty_state(canvas_, Icon::Check, "No lists yet",
                      "Shopping, packing, to-dos. Lists stay on Pocket and sync when linked.", 360);
      focus_.count = 1;
    } else {
      focus_.count = n + 1;
      for (int i = 0; i < n && row_y(list_top, i) + kFocusRowH < kBottomCtaY - 8; ++i) {
        const auto& L = data_.lists[static_cast<size_t>(i)];
        int done = 0;
        for (const auto& it : L.items) done += it.checked ? 1 : 0;
        char cnt[24];
        std::snprintf(cnt, sizeof(cnt), "%d/%d", done, static_cast<int>(L.items.size()));
        ui::row(canvas_, row_y(list_top, i), Row{L.title, RowKind::Nav, cnt}, i == focus_.index);
      }
    }
    ui::button(canvas_, kBottomCtaY, "New list", n == 0 || focus_.index == n, Icon::Plus, true);
  } else if (s == ScreenId::ListsDetail) {
    if (note_index_ < static_cast<int>(data_.lists.size())) {
      auto& L = data_.lists[static_cast<size_t>(note_index_)];
      ui::header(canvas_, L.title, true);
      const int top = below_title(kContentTop);
      focus_.count = static_cast<int>(L.items.size()) + 1;
      for (size_t i = 0; i < L.items.size(); ++i) {
        const int y = row_y(top, static_cast<int>(i));
        if (y + kFocusRowH >= kBottomCtaY - 8) break;
        ui::row(canvas_, y, Row{L.items[i].text, RowKind::Check, {}, L.items[i].checked},
                static_cast<int>(i) == focus_.index);
      }
      if (L.items.empty()) {
        canvas_.draw_text_wrapped(kSideMargin, top, kContentW, kWrapGap,
                                  "Empty list. Hold the side button and say an item.", Canvas::TextRole::Secondary,
                                  Gray::G0);
      }
      ui::button(canvas_, kBottomCtaY, "Hold side button to add",
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
    ptt_active_ = true;
    mic_capturing_ = true;
    if (audio_) audio_->start_capture();
    mark_content_dirty();
    return;
  }
  if (e == InputEvent::PttStop) {
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
        mark_list_focus_dirty(kContentTop + kTabBand, prev, focus_.index);
      } else {
        mark_content_dirty();
      }
    } else if (e == InputEvent::Select) {
      if (focus_.index < static_cast<int>(data_.notes.size())) {
        note_index_ = focus_.index;
        nav_.push(ScreenId::NotesDetail);
        after_nav();
      } else {
        Note n;
        n.id = std::to_string(now_ms_) + "-" + std::to_string(data_.notes.size() + 1);
        n.title = "Note";
        n.body = "";
        n.created_at = n.updated_at = now_ms_;
        data_.notes.push_back(n);
        note_index_ = static_cast<int>(data_.notes.size()) - 1;
        save_local_notes();
        nav_.push(ScreenId::NotesDetail);
        after_nav();
      }
    }
  } else if (s == ScreenId::NotesDetail) {
    focus_.count = 2;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      const int prev = focus_.index;
      focus_.move(e == InputEvent::Down ? 1 : -1);
      if (focus_.index == prev) return;
      mark_list_focus_dirty(bottom_action_y(1), prev, focus_.index);
    } else if (e == InputEvent::Select && focus_.index == 0) {
      if (!wifi_.connected()) {
        error_msg_ = "Hold the side button to capture. Text needs Wi‑Fi.";
      } else {
        error_msg_ = "Hold the side button to dictate.";
      }
      error_until_ms_ = now_ms_ + 3000;
      mark_content_dirty();
    } else if (e == InputEvent::Select && focus_.index == 1) {
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
        mark_list_focus_dirty(kContentTop + kTabBand, prev, focus_.index);
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
        L.id = std::to_string(now_ms_) + "-L" + std::to_string(data_.lists.size() + 1);
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
          mark_list_focus_dirty(below_title(kContentTop), prev, focus_.index);
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

static const char* kMonthName[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};

void App::render_clock() {
  draw_status_bar();
  static const char* kClockTabs[] = {"Clock", "Alarms", "Timers"};
  ui::tabs(canvas_, kContentTop, kClockTabs, 3, clock_tab_);
  ScreenId s = nav_.current();
  if (s == ScreenId::ClockFace || clock_tab_ == 0) {
    int h = 0, m = 0, wd = 0, mo = 0, d = 0;
    clock_.local_hm(h, m, wd, mo, d);
    char tbuf[16];
    if (!clock_.time_valid()) {
      std::snprintf(tbuf, sizeof(tbuf), "--:--");
    } else if (cfg_.time_format == 24) {
      std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d", h, m);
    } else {
      int h12 = h % 12;
      if (h12 == 0) h12 = 12;
      std::snprintf(tbuf, sizeof(tbuf), "%d:%02d", h12, m);
    }
    canvas_.draw_text_centered(kCanvasW / 2, kContentTop + kTabBand + 80, tbuf,
                               Canvas::TextRole::HugeClock, Gray::G0);
    // Local once synced (SNTP / RTC / NVS restore). Clear copy when unsynced or approximate.
    if (!clock_.time_valid()) {
      canvas_.draw_text_wrapped(
          kSideMargin, kFooterY - 4, kContentW, kWrapGap,
          wifi_.connected() ? "Syncing time…" : "Connect to Wi‑Fi once to set the time.",
          Canvas::TextRole::Secondary, Gray::G1);
    } else if (clock_.time_needs_network_sync()) {
      canvas_.draw_text_wrapped(
          kSideMargin, kFooterY - 4, kContentW, kWrapGap,
          wifi_.connected() ? "Syncing time…"
                            : "Time may be off after power-off — connect Wi‑Fi to sync.",
          Canvas::TextRole::Caption, Gray::G0);
      if (wd >= 0 && wd < 7) {
        static const char* kWd[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                    "Thursday", "Friday", "Saturday"};
        char datebuf[48];
        std::snprintf(datebuf, sizeof(datebuf), "%s, %s %d", kWd[wd], kMonthName[((mo % 12) + 12) % 12], d);
        canvas_.draw_text_centered(
            kCanvasW / 2,
            kContentTop + kTabBand + 80 + canvas_.text_height(Canvas::TextRole::HugeClock) + 16,
            datebuf, Canvas::TextRole::Secondary, Gray::G1);
      }
    } else if (wd >= 0 && wd < 7) {
      static const char* kWd[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                  "Thursday", "Friday", "Saturday"};
      char datebuf[48];
      std::snprintf(datebuf, sizeof(datebuf), "%s, %s %d", kWd[wd], kMonthName[((mo % 12) + 12) % 12], d);
      canvas_.draw_text_centered(kCanvasW / 2, kContentTop + kTabBand + 80 +
                                                   canvas_.text_height(Canvas::TextRole::HugeClock) + 16,
                                 datebuf, Canvas::TextRole::Secondary, Gray::G1);
    }
  } else if (clock_tab_ == 1) {
    ui::empty_state(canvas_, ui::Icon::Alarm, "No alarms yet",
                    "Alarms arrive in a free update. The clock keeps time offline.", 400);
  } else {
    ui::empty_state(canvas_, ui::Icon::Timer, "No timers yet",
                    "Timers arrive in a free update. The clock keeps time offline.", 400);
  }
}

void App::handle_clock(InputEvent e) {
  if (e == InputEvent::Back) {
    nav_.pop();
    after_nav();
    return;
  }
  if (e == InputEvent::Up || e == InputEvent::Down) {
    // Tab change redraws the body (clock face ↔ empty states) — content band, not full panel.
    clock_tab_ = e == InputEvent::Down ? (clock_tab_ + 1) % 3 : (clock_tab_ + 2) % 3;
    mark_content_dirty();
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
    for (int i = 0; i < focus_.count; ++i) {
      const auto& p = data_.passes[static_cast<size_t>(i)];
      ui::row(canvas_, row_y(list_top, i), ui::Row{p.title, ui::RowKind::Nav, p.type, false, Icon::Pass, true},
              i == focus_.index);
    }
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
      mark_list_focus_dirty(below_title(kContentTop), prev, focus_.index);
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
  if (data_.weather.city.empty() && !cfg_.weather_city.empty()) data_.weather.city = cfg_.weather_city;
  if (data_.weather.city.empty()) {
    ui::header(canvas_, "Weather", true);
    ui::empty_state(canvas_, ui::Icon::Weather, "No city set",
                    wifi_.connected() ? "Set your city in Companion, then choose Try again."
                                      : "Set your city in Companion when you're back online.",
                    360);
    focus_.count = 1;
    ui::button(canvas_, kBottomCtaY, "Try again", true, ui::Icon::Update, true);
    return;
  }
  ui::header(canvas_, data_.weather.city, true);
  {
    // Condition glyph, top right of the hero temperature.
    std::string cond = data_.weather.today_condition;
    for (auto& ch : cond) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    ui::Icon wi = ui::Icon::Weather;
    if (cond.find("rain") != std::string::npos || cond.find("drizzle") != std::string::npos ||
        cond.find("shower") != std::string::npos)
      wi = ui::Icon::Rain;
    else if (cond.find("cloud") != std::string::npos || cond.find("overcast") != std::string::npos ||
             cond.find("fog") != std::string::npos)
      wi = ui::Icon::Cloud;
    ui::icon(canvas_, wi, kCanvasW - kSideMargin - 48, below_title(kContentTop) + 44, 88, Gray::G0);
  }
  // Big serif temperature with the unit set small beside it.
  char tbuf[32];
  std::snprintf(tbuf, sizeof(tbuf), "%d\xC2\xB0", data_.weather.today_temp);
  const int temp_y = below_title(kContentTop) - 8;
  canvas_.draw_text(kSideMargin - 4, temp_y, tbuf, Canvas::TextRole::Hero, Gray::G0);
  const int tw = canvas_.text_width(tbuf, Canvas::TextRole::Hero);
  canvas_.draw_text(kSideMargin + tw + 2, temp_y + 22, cfg_.weather_units ? "C" : "F",
                    Canvas::TextRole::BodyStrong, Gray::G0);
  const int cond_y = temp_y + canvas_.text_height(Canvas::TextRole::Hero) + 4;
  canvas_.draw_text_fit(kSideMargin, cond_y, kContentW, data_.weather.today_condition,
                        Canvas::TextRole::Body, Gray::G0);
  // Forecast table: weekday · high / low · condition, on dotted rules.
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
    auto& d = data_.weather.days[i];
    const int ry = forecast_top + static_cast<int>(i) * kRowH;
    canvas_.dotted_hline(kSideMargin, ry - 10, kContentW, Gray::G0, 3);
    const int wd = weekday_of(d.date);
    const char* day = i == 0 ? "Today" : (wd >= 0 ? kDow[wd] : d.date.c_str());
    canvas_.draw_text(kSideMargin, ry, day, Canvas::TextRole::BodyStrong, Gray::G0);
    char hl[24];
    std::snprintf(hl, sizeof(hl), "%d\xC2\xB0 / %d\xC2\xB0", d.hi, d.lo);
    canvas_.draw_text(kSideMargin + 108, ry + 3, hl, Canvas::TextRole::Secondary, Gray::G0);
    canvas_.draw_text_fit(kSideMargin + 232, ry + 3, kContentW - 232, d.condition, Canvas::TextRole::Secondary,
                          Gray::G0);
  }
  const char* footer = wifi_.connected() && data_.weather.fetched_at != 0
                           ? "Updated · hold Select to refresh"
                           : "Offline · showing saved forecast";
  if (data_.weather.fetched_at == 0 && data_.weather.days.empty()) {
    footer = wifi_.connected() ? "Select to fetch forecast" : "Offline · no saved forecast yet";
  }
  ui::footer(canvas_, footer);
  focus_.count = 1;
  if (now_ms_ < error_until_ms_ && !error_msg_.empty()) ui::toast(canvas_, error_msg_);
}

void App::handle_weather(InputEvent e) {
  if (e == InputEvent::Back) {
    nav_.pop();
    after_nav();
    return;
  }
  if (e == InputEvent::Select) {
    weather_refresh();
  }
}

}  // namespace pocket
