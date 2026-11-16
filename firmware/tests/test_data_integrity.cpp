/** Data integrity: JSON parser edge cases and lossless Notes/Lists/Alarms persistence. */
#include "pocket/app.hpp"
#include "pocket/json_lite.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
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
  uint32_t now_ms() override { return t += 50; }
  void local_hm(int& h, int& m, int& wd, int& mo, int& d) override {
    h = 7;
    m = 0;
    wd = 1;
    mo = 10;
    d = 16;
  }
};
struct TWifi : PlatformWifi {
  std::vector<std::string> scan() override { return {}; }
  bool connect(const std::string&, const std::string&) override { return true; }
  bool connected() const override { return false; }
  bool start_provision(const std::string&, std::string*, std::string*) override { return false; }
  void stop_provision() override {}
  bool take_provision_credentials(std::string*, std::string*) override { return false; }
  std::string provision_ap_ssid() const override { return {}; }
  std::string provision_ap_password() const override { return {}; }
};
struct TCloud : PlatformCloud {
  std::string create_pair_session(const std::string&) override { return {}; }
  std::string pair_status(const std::string&) override { return "pending"; }
  void refresh_entitlement(DeviceConfig&) override {}
  std::string stt_transcribe(const std::vector<uint8_t>&) override { return {}; }
};
struct TDisp : PlatformDisplay {
  void present(const Canvas&, RefreshMode) override {}
};
struct TStorage : PlatformStorage {
  std::string root = "/tmp/pocket-data-integrity-test";
  std::string data_root() override { return root; }
  bool data_ensure_root() override {
    ::mkdir(root.c_str(), 0755);
    return true;
  }
};

int main() {
  // JSON parser.
  {
    JsonValue v;
    CHECK(json_parse(R"({"a":"x \"q\" {b} }","n":-12.5,"t":true,"z":null,"arr":[1,{"k":"é🎉"}]})", v));
    CHECK(v.get_string("a") == "x \"q\" {b} }");
    CHECK(v.get_number("n") == -12.5);
    CHECK(v.get_bool("t"));
    const JsonValue* arr = v.get("arr");
    CHECK(arr && arr->items.size() == 2);
    CHECK(arr && arr->items[1].get_string("k") == "\xC3\xA9\xF0\x9F\x8E\x89");  // é🎉
    CHECK(!json_parse("{\"a\":1,}", v));
    CHECK(!json_parse("[1,2", v));
    CHECK(json_escape_str("a\"b\\c\n") == "a\\\"b\\\\c\\n");
  }

  std::system("rm -rf /tmp/pocket-data-integrity-test");
  const std::string tricky = "He said \"hi\" {braces} } and \"pending\": no \xC3\xA9 \xF0\x9F\x8E\x89\nline 2";
  {
    MemoryConfigStore store;
    store.mut().onboarding_complete = true;
    store.mut().pin_length = 4;
    set_pin(store.mut(), "0000");
    TClock clock;
    TWifi wifi;
    TCloud cloud;
    TDisp disp;
    TStorage storage;
    App app(store, clock, wifi, cloud, disp, &storage);
    app.boot();
    auto& d = app.host_data();
    Note n;
    n.id = "n1";
    n.title = "Title } {";
    n.body = tricky;
    d.notes.push_back(n);
    TodoList L;
    L.id = "l1";
    L.title = "Groceries";
    L.items = {{"i1", "eggs } bacon", false, 0, {}}, {"i2", "\"quoted\" milk", true, 0, {}}};
    d.lists.push_back(L);
    // Drive a save through the public flow: toggle an item on the list screen.
    app.host_goto(ScreenId::ListsDetail, 1);
    app.handle(InputEvent::Select);  // toggles item 0 → saves lists
    app.host_goto(ScreenId::NotesDetail);
  }
  // Persist notes too via a fresh app that saves on boot path: write directly through App API.
  {
    MemoryConfigStore store;
    store.mut().onboarding_complete = true;
    TClock clock;
    TWifi wifi;
    TCloud cloud;
    TDisp disp;
    TStorage storage;
    App app(store, clock, wifi, cloud, disp, &storage);
    app.boot();
    const auto& d = app.host_data();
    CHECK(d.lists.size() == 1);
    if (!d.lists.empty()) {
      CHECK(d.lists[0].items.size() == 2);  // used to reload with 0 items
      if (d.lists[0].items.size() == 2) {
        CHECK(d.lists[0].items[0].text == "eggs } bacon");
        CHECK(d.lists[0].items[1].text == "\"quoted\" milk");
      }
    }
  }

  // Notes round-trip through the file format directly.
  {
    std::string out = "[{\"id\":\"n1\",\"title\":\"" + json_escape_str("Title } {") + "\",\"body\":\"" +
                      json_escape_str(tricky) + "\",\"created_at\":1,\"updated_at\":2}]";
    ::mkdir("/tmp/pocket-data-integrity-test/notes", 0755);
    CHECK(write_file_atomic("/tmp/pocket-data-integrity-test/notes/index.json", out));
    MemoryConfigStore store;
    store.mut().onboarding_complete = true;
    TClock clock;
    TWifi wifi;
    TCloud cloud;
    TDisp disp;
    TStorage storage;
    App app(store, clock, wifi, cloud, disp, &storage);
    app.boot();
    const auto& d = app.host_data();
    CHECK(d.notes.size() == 1);
    if (!d.notes.empty()) {
      CHECK(d.notes[0].title == "Title } {");
      CHECK(d.notes[0].body == tricky);
    }
  }

  // Corrupt file: keep a .corrupt copy, don't crash.
  {
    CHECK(write_file_atomic("/tmp/pocket-data-integrity-test/notes/index.json", "[{\"id\":\"n1\",\"title\":"));
    MemoryConfigStore store;
    store.mut().onboarding_complete = true;
    TClock clock;
    TWifi wifi;
    TCloud cloud;
    TDisp disp;
    TStorage storage;
    App app(store, clock, wifi, cloud, disp, &storage);
    app.boot();
    std::string kept;
    CHECK(read_file("/tmp/pocket-data-integrity-test/notes/index.json.corrupt", kept) && !kept.empty());
  }

  // Interrupted atomic write: only .tmp left → recovered.
  {
    std::remove("/tmp/pocket-data-integrity-test/x.json");
    CHECK(write_file_atomic("/tmp/pocket-data-integrity-test/x.json.tmp.src", "[1]"));
    std::rename("/tmp/pocket-data-integrity-test/x.json.tmp.src", "/tmp/pocket-data-integrity-test/x.json.tmp");
    std::string got;
    CHECK(read_file_recover("/tmp/pocket-data-integrity-test/x.json", got) && got == "[1]");
  }

  if (failures == 0) std::printf("test_data_integrity: OK\n");
  return failures == 0 ? 0 : 1;
}
