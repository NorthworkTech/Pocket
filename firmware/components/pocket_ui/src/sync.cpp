/**
 * Notes & Lists sync with Pocket Cloud / Companion.
 *
 * The device keeps a small "last agreed" record (sync/state.json: id → content hash). Each sync
 *   1. pushes anything whose content differs from that record (new or edited here), plus
 *      tombstones for ids in the record that are gone locally (deleted here),
 *   2. lets the server resolve conflicts last-write-wins,
 *   3. adopts the server's resulting set, and records it as the new agreement.
 * Items still waiting on a voice transcript stay local and are left alone until they're text.
 */
#include "pocket/app.hpp"
#include "pocket/json_lite.hpp"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <sys/stat.h>

namespace pocket {

namespace {

uint64_t fnv1a(std::string_view s, uint64_t h = 1469598103934665603ull) {
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex64(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
  return b;
}

std::string note_hash(const Note& n) { return hex64(fnv1a(n.body, fnv1a(n.title + '\x1f'))); }

std::string list_hash(const TodoList& L) {
  uint64_t h = fnv1a(L.title + '\x1f');
  for (const auto& it : L.items) {
    h = fnv1a(it.id + '\x1e' + it.text + (it.checked ? "\x01" : "\x00"), h);
  }
  return hex64(h);
}

bool list_has_pending(const TodoList& L) {
  for (const auto& it : L.items)
    if (!it.pending_pcm.empty()) return true;
  return false;
}

std::string iso_utc(int64_t unix_s) {
  const time_t t = static_cast<time_t>(unix_s);
  struct tm g {};
  gmtime_r(&t, &g);
  char b[32];
  std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S.000Z", &g);
  return b;
}

using HashMap = std::map<std::string, std::string>;

void read_state(const std::string& path, HashMap& notes, HashMap& lists) {
  std::string body;
  if (!read_file_recover(path, body)) return;
  JsonValue v;
  if (!json_parse(body, v) || !v.is_object()) return;
  auto load = [](const JsonValue* o, HashMap& out) {
    if (!o || !o->is_object()) return;
    for (const auto& m : o->members)
      if (m.second.type == JsonValue::Type::String) out[m.first] = m.second.str;
  };
  load(v.get("notes"), notes);
  load(v.get("lists"), lists);
}

void write_state(const std::string& path, const HashMap& notes, const HashMap& lists) {
  std::string out = "{\"notes\":{";
  bool first = true;
  for (const auto& kv : notes) {
    out += std::string(first ? "" : ",") + "\"" + json_escape_str(kv.first) + "\":\"" + kv.second + "\"";
    first = false;
  }
  out += "},\"lists\":{";
  first = true;
  for (const auto& kv : lists) {
    out += std::string(first ? "" : ",") + "\"" + json_escape_str(kv.first) + "\":\"" + kv.second + "\"";
    first = false;
  }
  out += "}}";
  (void)write_file_atomic(path, out);
}

}  // namespace

std::string App::new_local_id(const char* prefix) {
  // Unique across devices: wall time (or uptime) + device + counter. Server ids are UUIDs.
  const int64_t t = clock_.time_valid() ? static_cast<int64_t>(std::time(nullptr)) : 0;
  std::string dev;
  for (char c : cfg_.device_id)
    if (std::isalnum(static_cast<unsigned char>(c)) && dev.size() < 8) dev.push_back(c);
  return std::string(prefix) + std::to_string(t) + "-" + std::to_string(now_ms_) + "-" + dev + "-" +
         std::to_string(++id_counter_);
}

bool App::cloud_sync_possible() const {
  return cfg_.onboarding_complete && cfg_.companion_linked && cfg_.cloud_entitled && wifi_.connected() &&
         !cfg_.device_id.empty() && clock_.time_valid() && storage_ != nullptr;
}

bool App::cloud_sync(bool show_progress) {
  if (!cloud_sync_possible()) {
    if (!cfg_.companion_linked) sync_status_ = "Link Companion to sync notes and lists.";
    else if (!cfg_.cloud_entitled) sync_status_ = "Pocket Cloud keeps notes and lists in sync.";
    else if (!wifi_.connected()) sync_status_ = "Offline \xC2\xB7 changes sync when Wi-Fi is back.";
    return false;
  }
  if (!storage_->data_ensure_root()) return false;
  last_sync_attempt_ms_ = now_ms_;
  const std::string dir = storage_->data_root() + "/sync";
  ::mkdir(dir.c_str(), 0755);
  const std::string state_path = dir + "/state.json";
  HashMap s_notes, s_lists;
  read_state(state_path, s_notes, s_lists);

  const std::string now_iso = iso_utc(static_cast<int64_t>(std::time(nullptr)));
  bool local_changed = false;

  // ---- 1. Push local changes --------------------------------------------------------
  std::string body = "{\"sd_present\":";
  body += sd_present() ? "true" : "false";
  body += ",\"notes\":[";
  int pushed = 0;
  bool first = true;
  for (const auto& n : data_.notes) {
    if (!n.pending_pcm.empty()) continue;
    const auto it = s_notes.find(n.id);
    if (it != s_notes.end() && it->second == note_hash(n)) continue;
    body += std::string(first ? "" : ",") + "{\"id\":\"" + json_escape_str(n.id) + "\",\"title\":\"" +
            json_escape_str(n.title) + "\",\"body\":\"" + json_escape_str(n.body) + "\",\"created_at\":\"" +
            now_iso + "\",\"updated_at\":\"" + now_iso + "\",\"deleted_at\":null}";
    first = false;
    ++pushed;
  }
  for (const auto& kv : s_notes) {
    const bool here = std::any_of(data_.notes.begin(), data_.notes.end(), [&](const Note& n) { return n.id == kv.first; });
    if (here) continue;
    body += std::string(first ? "" : ",") + "{\"id\":\"" + json_escape_str(kv.first) +
            "\",\"title\":\"\",\"body\":\"\",\"created_at\":\"" + now_iso + "\",\"updated_at\":\"" + now_iso +
            "\",\"deleted_at\":\"" + now_iso + "\"}";
    first = false;
    ++pushed;
  }
  body += "],\"lists\":[";
  first = true;
  for (auto& L : data_.lists) {
    if (list_has_pending(L)) continue;
    for (auto& item : L.items) {
      if (item.id.empty()) {
        item.id = new_local_id("i");
        local_changed = true;
      }
    }
    const auto it = s_lists.find(L.id);
    if (it != s_lists.end() && it->second == list_hash(L)) continue;
    body += std::string(first ? "" : ",") + "{\"id\":\"" + json_escape_str(L.id) + "\",\"title\":\"" +
            json_escape_str(L.title) + "\",\"items\":[";
    for (size_t j = 0; j < L.items.size(); ++j) {
      const auto& item = L.items[j];
      body += std::string(j ? "," : "") + "{\"id\":\"" + json_escape_str(item.id) + "\",\"text\":\"" +
              json_escape_str(item.text) + "\",\"checked\":" + (item.checked ? "true" : "false") +
              ",\"updated_at\":\"" + now_iso + "\"}";
    }
    body += "],\"created_at\":\"" + now_iso + "\",\"updated_at\":\"" + now_iso + "\",\"deleted_at\":null}";
    first = false;
    ++pushed;
  }
  for (const auto& kv : s_lists) {
    const bool here =
        std::any_of(data_.lists.begin(), data_.lists.end(), [&](const TodoList& L) { return L.id == kv.first; });
    if (here) continue;
    body += std::string(first ? "" : ",") + "{\"id\":\"" + json_escape_str(kv.first) +
            "\",\"title\":\"\",\"items\":[],\"created_at\":\"" + now_iso + "\",\"updated_at\":\"" + now_iso +
            "\",\"deleted_at\":\"" + now_iso + "\"}";
    first = false;
    ++pushed;
  }
  body += "]}";
  if (local_changed) save_local_lists();

  if (show_progress) show_status("Syncing with Companion\xE2\x80\xA6");
  int status = 0;
  const std::string resp = cloud_.device_sync_json(cfg_.device_id, body, &status);
  if (status == 402) {
    cfg_.cloud_entitled = false;
    sync_status_ = "Pocket Cloud keeps notes and lists in sync.";
    return false;
  }
  JsonValue root;
  if (status != 200 || resp.empty() || !json_parse(resp, root) || !root.is_object()) {
    sync_status_ = "Couldn't sync \xC2\xB7 will retry.";
    return false;
  }
  const JsonValue* rnotes = root.get("notes");
  const JsonValue* rlists = root.get("lists");
  if (!rnotes || !rnotes->is_array() || !rlists || !rlists->is_array()) {
    sync_status_ = "Couldn't sync \xC2\xB7 will retry.";
    return false;
  }

  // ---- 2. Adopt the server's set -----------------------------------------------------
  HashMap n_notes, n_lists;
  bool notes_dirty = false, lists_dirty = false;
  std::vector<std::string> server_note_ids, server_list_ids;
  for (const auto& o : rnotes->items) {
    if (!o.is_object()) continue;
    const std::string id = o.get_string("id");
    if (id.empty()) continue;
    server_note_ids.push_back(id);
    const JsonValue* del = o.get("deleted_at");
    const bool deleted = del && del->type == JsonValue::Type::String && !del->str.empty();
    auto local = std::find_if(data_.notes.begin(), data_.notes.end(), [&](const Note& n) { return n.id == id; });
    if (local != data_.notes.end() && !local->pending_pcm.empty()) {
      continue;  // voice clip still transcribing — keep local, sync it next time
    }
    if (deleted) {
      if (local != data_.notes.end()) {
        data_.notes.erase(local);
        notes_dirty = true;
      }
      continue;
    }
    Note n;
    n.id = id;
    n.title = o.get_string("title");
    n.body = o.get_string("body");
    if (local == data_.notes.end()) {
      n.created_at = n.updated_at = now_ms_;
      data_.notes.push_back(n);
      notes_dirty = true;
    } else if (local->title != n.title || local->body != n.body) {
      local->title = n.title;
      local->body = n.body;
      local->updated_at = now_ms_;
      notes_dirty = true;
    }
    n_notes[id] = note_hash(n);
  }
  // Previously agreed notes the server no longer knows (purged tombstones) → gone.
  for (auto it = data_.notes.begin(); it != data_.notes.end();) {
    const bool on_server = std::find(server_note_ids.begin(), server_note_ids.end(), it->id) != server_note_ids.end();
    if (!on_server && it->pending_pcm.empty() && s_notes.count(it->id) && s_notes[it->id] == note_hash(*it)) {
      it = data_.notes.erase(it);
      notes_dirty = true;
    } else {
      ++it;
    }
  }
  for (const auto& o : rlists->items) {
    if (!o.is_object()) continue;
    const std::string id = o.get_string("id");
    if (id.empty()) continue;
    server_list_ids.push_back(id);
    const JsonValue* del = o.get("deleted_at");
    const bool deleted = del && del->type == JsonValue::Type::String && !del->str.empty();
    auto local = std::find_if(data_.lists.begin(), data_.lists.end(), [&](const TodoList& L) { return L.id == id; });
    if (local != data_.lists.end() && list_has_pending(*local)) continue;
    if (deleted) {
      if (local != data_.lists.end()) {
        data_.lists.erase(local);
        lists_dirty = true;
      }
      continue;
    }
    TodoList L;
    L.id = id;
    L.title = o.get_string("title");
    L.shared = o.get_bool("shared");
    L.owner = o.get_string("owner");
    if (const JsonValue* items = o.get("items"); items && items->is_array()) {
      for (const auto& io : items->items) {
        if (!io.is_object()) continue;
        ListItem item;
        item.id = io.get_string("id");
        item.text = io.get_string("text");
        item.checked = io.get_bool("checked");
        if (!item.text.empty()) L.items.push_back(std::move(item));
      }
    }
    if (local == data_.lists.end()) {
      L.updated_at = now_ms_;
      data_.lists.push_back(L);
      lists_dirty = true;
    } else {
      if (local->shared != L.shared || local->owner != L.owner) {
        local->shared = L.shared;
        local->owner = L.owner;
        lists_dirty = true;
      }
      if (list_hash(*local) != list_hash(L)) {
        local->title = L.title;
        local->items = L.items;
        local->updated_at = now_ms_;
        lists_dirty = true;
      }
    }
    n_lists[id] = list_hash(L);
  }
  for (auto it = data_.lists.begin(); it != data_.lists.end();) {
    const bool on_server = std::find(server_list_ids.begin(), server_list_ids.end(), it->id) != server_list_ids.end();
    if (!on_server && !list_has_pending(*it) && s_lists.count(it->id) && s_lists[it->id] == list_hash(*it)) {
      it = data_.lists.erase(it);
      lists_dirty = true;
    } else {
      ++it;
    }
  }
  // Persist whenever anything moved either way, so disk always matches the recorded agreement.
  if (notes_dirty || pushed) save_local_notes();
  if (lists_dirty || pushed) save_local_lists();
  write_state(state_path, n_notes, n_lists);
  // Detail screens index by position — keep them pointing at something valid.
  const ScreenId s = nav_.current();
  if ((s == ScreenId::NotesDetail && note_index_ >= static_cast<int>(data_.notes.size())) ||
      (s == ScreenId::ListsDetail && note_index_ >= static_cast<int>(data_.lists.size()))) {
    if (!nav_.pop()) nav_.reset(ScreenId::Home);
    after_nav();
  } else if (notes_dirty || lists_dirty) {
    mark_content_dirty();
  }
  last_sync_ok_ms_ = now_ms_;
  sync_status_ = "Synced with Companion";
  return true;
}

void App::maybe_cloud_sync() {
  if (!cloud_sync_possible()) return;
  const ScreenId s = nav_.current();
  // Quiet background sync only where nobody is mid-edit and nothing visibly jumps.
  const bool quiet = s == ScreenId::Lock || s == ScreenId::Home;
  if (!quiet) return;
  uint32_t every = s == ScreenId::Lock ? 15u * 60u * 1000u : 5u * 60u * 1000u;
  if (battery_saver_) every *= 4;  // low battery: background sync rarely; opening Notes still syncs
  if (last_sync_attempt_ms_ != 0 && now_ms_ - last_sync_attempt_ms_ < every) return;
  if (dirty_ || display_.busy()) return;
  (void)cloud_sync(false);
}

}  // namespace pocket
