/** Notes & Lists sync with Companion: push local edits/deletes, adopt cloud edits/deletes,
 * leave pending voice clips alone, report SD, honor entitlement. */
#include "pocket/app.hpp"
#include "pocket/json_lite.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace pocket;

static int failures = 0;
#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures; \
    } \
  } while (0)

struct TClock : PlatformClock {
  uint32_t t = 1000;
  uint32_t now_ms() override { return t; }
  void local_hm(int& h, int& m, int& wd, int& mo, int& d) override { h = 10; m = 0; wd = 1; mo = 3; d = 24; }
};
struct TWifi : PlatformWifi {
  std::vector<std::string> scan() override { return {}; }
  bool connect(const std::string&, const std::string&) override { return true; }
  bool connected() const override { return true; }
  bool start_provision(const std::string&, std::string*, std::string*) override { return false; }
  void stop_provision() override {}
  bool take_provision_credentials(std::string*, std::string*) override { return false; }
  std::string provision_ap_ssid() const override { return {}; }
  std::string provision_ap_password() const override { return {}; }
};

struct SNote { std::string title, body; bool deleted = false; };
struct SItem { std::string id, text; bool checked = false; };
struct SList { std::string title; std::vector<SItem> items; bool deleted = false; };

/** Minimal Pocket Cloud: device pushes win (they carry "now"), response = full set. */
struct TCloud : PlatformCloud {
  std::map<std::string, SNote> notes;
  std::map<std::string, SList> lists;
  int pushes_notes = 0, pushes_lists = 0, sd = -1, last_sd_body = -1;
  bool entitled = true;
  std::string create_pair_session(const std::string&) override { return {}; }
  std::string pair_status(const std::string&) override { return "pending"; }
  void refresh_entitlement(DeviceConfig&) override {}
  std::string stt_transcribe(const std::vector<uint8_t>&) override { return {}; }
  void set_sd_present(int s) override { sd = s; }
  std::string device_sync_json(const std::string&, const std::string& body, int* status) override {
    if (!entitled) { *status = 402; return R"({"message":"no"})"; }
    JsonValue v;
    if (!json_parse(body, v)) { *status = 400; return "{}"; }
    if (const JsonValue* s = v.get("sd_present")) last_sd_body = s->boolean ? 1 : 0;
    for (const auto& o : v.get("notes")->items) {
      ++pushes_notes;
      SNote& n = notes[o.get_string("id")];
      const JsonValue* d = o.get("deleted_at");
      n.deleted = d && d->type == JsonValue::Type::String;
      if (!n.deleted) { n.title = o.get_string("title"); n.body = o.get_string("body"); }
    }
    for (const auto& o : v.get("lists")->items) {
      ++pushes_lists;
      SList& L = lists[o.get_string("id")];
      const JsonValue* d = o.get("deleted_at");
      L.deleted = d && d->type == JsonValue::Type::String;
      if (!L.deleted) {
        L.title = o.get_string("title");
        L.items.clear();
        for (const auto& io : o.get("items")->items)
          L.items.push_back({io.get_string("id"), io.get_string("text"), io.get_bool("checked")});
      }
    }
    std::string out = R"({"server_time":"2026-11-17T00:00:00.000Z","notes":[)";
    bool first = true;
    for (auto& kv : notes) {
      out += std::string(first ? "" : ",") + "{\"id\":\"" + json_escape_str(kv.first) + "\",\"title\":\"" +
             json_escape_str(kv.second.title) + "\",\"body\":\"" + json_escape_str(kv.second.body) +
             "\",\"deleted_at\":" + (kv.second.deleted ? "\"2026-11-17T00:00:00.000Z\"" : "null") + "}";
      first = false;
    }
    out += "],\"lists\":[";
    first = true;
    for (auto& kv : lists) {
      out += std::string(first ? "" : ",") + "{\"id\":\"" + json_escape_str(kv.first) + "\",\"title\":\"" +
             json_escape_str(kv.second.title) + "\",\"items\":[";
      for (size_t j = 0; j < kv.second.items.size(); ++j) {
        const auto& it = kv.second.items[j];
        out += std::string(j ? "," : "") + "{\"id\":\"" + it.id + "\",\"text\":\"" + json_escape_str(it.text) +
               "\",\"checked\":" + (it.checked ? "true" : "false") + ",\"updated_at\":\"x\"}";
      }
      out += std::string("],\"deleted_at\":") + (kv.second.deleted ? "\"2026-11-17T00:00:00.000Z\"" : "null") + "}";
      first = false;
    }
    out += "]}";
    *status = 200;
    return out;
  }
};
struct TDisp : PlatformDisplay {
  void present(const Canvas&, RefreshMode) override {}
};
struct TStorage : PlatformStorage {
  std::string root = "/tmp/pocket-sync-test";
  bool sd = true;
  bool present() override { return sd; }
  std::string data_root() override { return root; }
  bool data_ensure_root() override { return std::system(("mkdir -p " + root).c_str()) == 0; }
};

static const Note* find_note(App& app, const std::string& id) {
  for (const auto& n : app.host_data().notes)
    if (n.id == id) return &n;
  return nullptr;
}

int main() {
  MemoryConfigStore store;
  auto& cfg = store.mut();
  cfg.onboarding_complete = true;
  cfg.companion_linked = true;
  cfg.cloud_entitled = true;
  cfg.device_id = "dev-1234abcd";
  cfg.pin_length = 4;
  set_pin(cfg, "0000");
  TClock clock; TWifi wifi; TCloud cloud; TDisp disp; TStorage storage;
  std::system(("rm -rf " + storage.root + " && mkdir -p " + storage.root).c_str());
  App app(store, clock, wifi, cloud, disp, &storage, nullptr);
  app.boot();

  auto& d = app.host_data();
  d.notes.push_back({"n-local", "Groceries", "Oat milk", 0, 0, {}});
  d.notes.push_back({"n-voice", "Voice note", "(transcribing)", 0, 0, "clip1.pcm"});
  TodoList L;
  L.id = "L-1";
  L.title = "Packing";
  L.items = {{"", "Boots", false, 0, {}}, {"i2", "Rain shell", true, 0, {}}};
  d.lists.push_back(L);
  cloud.notes["n-cloud"] = {"From phone", "Typed in Companion"};

  // First sync: local up, cloud down; pending voice note stays local only.
  CHECK(app.host_cloud_sync());
  CHECK(cloud.notes.count("n-local") && cloud.notes["n-local"].body == "Oat milk");
  CHECK(!cloud.notes.count("n-voice"));
  CHECK(find_note(app, "n-voice") != nullptr);
  CHECK(find_note(app, "n-cloud") && find_note(app, "n-cloud")->body == "Typed in Companion");
  CHECK(cloud.lists.count("L-1") && cloud.lists["L-1"].items.size() == 2);
  CHECK(!cloud.lists["L-1"].items[0].id.empty());  // items get stable ids
  CHECK(cloud.last_sd_body == 1);

  // Nothing changed → nothing pushed.
  cloud.pushes_notes = cloud.pushes_lists = 0;
  CHECK(app.host_cloud_sync());
  CHECK(cloud.pushes_notes == 0 && cloud.pushes_lists == 0);

  // Companion edits + deletes; device adopts.
  cloud.notes["n-local"].body = "Oat milk, lemons";
  cloud.notes["n-cloud"].deleted = true;
  cloud.lists["L-1"].items[1].checked = false;
  CHECK(app.host_cloud_sync());
  CHECK(find_note(app, "n-local") && find_note(app, "n-local")->body == "Oat milk, lemons");
  CHECK(find_note(app, "n-cloud") == nullptr);
  CHECK(!app.host_data().lists.empty() && !app.host_data().lists[0].items[1].checked);
  CHECK(cloud.pushes_notes == 0);  // adopting isn't echoed back

  // Device edits one, deletes another → pushed as edit + tombstone.
  for (auto& n : d.notes)
    if (n.id == "n-local") n.body = "Edited on Pocket";
  d.lists.clear();
  CHECK(app.host_cloud_sync());
  CHECK(cloud.notes["n-local"].body == "Edited on Pocket");
  CHECK(cloud.lists["L-1"].deleted);
  CHECK(app.host_data().lists.empty());

  // Survives a reboot: state on disk, no spurious re-push.
  App again(store, clock, wifi, cloud, disp, &storage, nullptr);
  again.boot();
  cloud.pushes_notes = cloud.pushes_lists = 0;
  CHECK(again.host_cloud_sync());
  CHECK(cloud.pushes_notes == 0 && cloud.pushes_lists == 0);

  // Lapsed Pocket Cloud → stop trying, say why.
  cloud.entitled = false;
  CHECK(!again.host_cloud_sync());
  CHECK(!again.config().cloud_entitled);

  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::puts("test_companion_sync OK");
  return 0;
}
