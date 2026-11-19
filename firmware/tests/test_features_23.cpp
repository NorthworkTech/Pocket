/** PocketOS 2.3: voice list commands, reading speed learning, calendar, battery saver,
 * backup & restore, bedtime face, Units in Weather. */
#include "pocket/app.hpp"
#include "pocket/voice_commands.hpp"
#include <cstdio>
#include <cstdlib>
#include <ctime>
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
  int batt = 80;
  int h = 10, m = 0;
  uint32_t now_ms() override { return t; }
  void local_hm(int& hh, int& mm, int& wd, int& mo, int& d) override { hh = h; mm = m; wd = 3; mo = 10; d = 19; }
  int battery_percent() override { return batt; }
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
struct TCloud : PlatformCloud {
  std::string calendar, last_backup, latest;
  int backups = 0;
  std::string create_pair_session(const std::string&) override { return {}; }
  std::string pair_status(const std::string&) override { return "pending"; }
  void refresh_entitlement(DeviceConfig&) override {}
  std::string stt_transcribe(const std::vector<uint8_t>&) override { return {}; }
  std::string calendar_json(const std::string&) override { return calendar; }
  bool post_backup(const std::string&, const std::string& b) override {
    ++backups;
    last_backup = b;
    return true;
  }
  std::string backup_latest_json(const std::string&) override { return latest; }
};
struct TDisp : PlatformDisplay {
  void present(const Canvas&, RefreshMode) override {}
};
struct TStorage : PlatformStorage {
  std::string root = "/tmp/pocket-f23-test";
  bool present() override { return false; }
  uint64_t free_bytes(const std::string&) override { return 64ull << 20; }
  std::string data_root() override { return root; }
  bool data_ensure_root() override { return std::system(("mkdir -p " + root).c_str()) == 0; }
};

static std::string iso(time_t t) {
  char b[32];
  struct tm g {};
  gmtime_r(&t, &g);
  std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S.000Z", &g);
  return b;
}

int main() {
  // ---- Voice command parsing (pure) ----
  ListCommand c;
  CHECK(parse_list_command("Add milk and eggs to groceries.", c) && c.list == "groceries" && c.items.size() == 2 &&
        c.items[0] == "Milk" && c.items[1] == "Eggs");
  CHECK(parse_list_command("put AA batteries on my hardware list", c) && c.list == "hardware" &&
        c.items[0] == "AA batteries");
  CHECK(parse_list_command("add toothpaste, soap, and shampoo to the shopping list", c) && c.items.size() == 3);
  CHECK(!parse_list_command("Remember to call Sam", c));
  CHECK(list_name_matches("Groceries", "grocery"));
  CHECK(list_name_matches("Packing list", "my packing"));

  std::system("rm -rf /tmp/pocket-f23-test");
  MemoryConfigStore store;
  store.mut().onboarding_complete = true;
  store.mut().companion_linked = true;
  store.mut().device_id = "dev-23";
  store.mut().pin_length = 4;
  set_pin(store.mut(), "0000");
  store.mut().idle_lock_s = 3600;
  TClock clock; TWifi wifi; TCloud cloud; TDisp disp; TStorage storage;
  App app(store, clock, wifi, cloud, disp, &storage, nullptr);
  app.boot();

  // ---- "add … to …" lands on the list (created when missing) ----
  TodoList g;
  g.id = "L1";
  g.title = "Groceries";
  app.host_data().lists.push_back(g);
  CHECK(app.host_list_command("Add oat milk and lemons to grocery list"));
  CHECK(app.host_data().lists[0].items.size() == 2);
  CHECK(app.host_list_command("add rope to camping"));
  CHECK(app.host_data().lists.size() == 2 && app.host_data().lists[1].title == "Camping");
  CHECK(!app.host_list_command("Dentist moved to Friday"));

  // ---- Reading speed is learned from page turns ----
  std::string text;
  for (int i = 0; i < 4000; ++i) text += "word ";
  app.host_goto(ScreenId::ReadingBook);
  app.host_open_book(text, 0);
  CHECK(app.host_minutes_left() < 0);  // not enough samples yet
  for (int i = 0; i < 6; ++i) {
    clock.t += 30000;                     // 30 s per page
    app.handle(InputEvent::Select);       // Next (focus 1) → records the turn
  }
  CHECK(app.host_wpm() > 60 && app.host_wpm() < 900);
  CHECK(app.host_minutes_left() > 0);
  const int before = app.host_minutes_left();
  clock.t += 30000;
  app.handle(InputEvent::Select);
  CHECK(app.host_minutes_left() <= before);  // fewer words left → less time

  // ---- Calendar: next event, cached for offline ----
  const time_t now = std::time(nullptr);
  cloud.calendar = std::string(R"({"events":[{"title":"Old","start":")") + iso(now - 7200) + R"(","end":")" +
                   iso(now - 3600) + R"(","all_day":false,"location":""},{"title":"Dentist","start":")" +
                   iso(now + 3600) + R"(","end":")" + iso(now + 5400) + R"(","all_day":false,"location":"Main St"}]})";
  app.host_fetch_calendar();
  std::string line, meta;
  CHECK(app.host_next_event(&line, &meta));
  CHECK(line.find("Dentist") != std::string::npos);
  CHECK(meta.find("Main St") != std::string::npos);
  {
    App offline(store, clock, wifi, cloud, disp, &storage, nullptr);
    offline.boot();  // loads calendar.json
    CHECK(offline.host_next_event(&line, nullptr) && line.find("Dentist") != std::string::npos);
  }

  // ---- Battery saver on at 15 %, off again at 20 % ----
  clock.batt = 14;
  clock.t += 60000;
  app.tick(clock.t);
  CHECK(app.host_saver());
  clock.batt = 18;
  clock.t += 60000;
  app.tick(clock.t);
  CHECK(app.host_saver());
  clock.batt = 25;
  clock.t += 60000;
  app.tick(clock.t);
  CHECK(!app.host_saver());

  // ---- Backup round trip (never the PIN) ----
  app.host_data().alarms = {{"a1", 6 * 60 + 45, true, "Run"}};
  app.mut_config().quiet_enabled = true;
  const int b0 = cloud.backups;  // the daily background backup may already have run
  app.host_backup();
  CHECK(cloud.backups == b0 + 1);
  CHECK(cloud.last_backup.find("\"Run\"") != std::string::npos);
  CHECK(cloud.last_backup.find("pin") == std::string::npos);
  {
    MemoryConfigStore fresh;
    fresh.mut().onboarding_complete = true;
    fresh.mut().device_id = "dev-new";
    TStorage st2;
    st2.root = "/tmp/pocket-f23-test/new";
    App neo(fresh, clock, wifi, cloud, disp, &st2, nullptr);
    neo.boot();
    CHECK(neo.host_apply_backup("{\"backup\":" + cloud.last_backup + "}"));
    CHECK(neo.host_data().alarms.size() == 1 && neo.host_data().alarms[0].label == "Run");
    CHECK(neo.config().quiet_enabled);
  }

  // ---- Restore is offered during setup when a backup exists ----
  {
    MemoryConfigStore s3;
    s3.mut().device_id = "dev-3";
    TStorage st3;
    st3.root = "/tmp/pocket-f23-test/three";
    struct PairCloud : TCloud {
      std::string pair_status(const std::string&) override { return "claimed"; }
      std::string create_pair_session(const std::string&) override { return "ABCD2345"; }
    } pc;
    pc.latest = "{\"backup\":{\"id\":\"b1\",\"created_at\":\"2026-11-18T10:00:00Z\",\"device_name\":\"Old Pocket\"," +
                std::string("\"settings\":{\"quiet_enabled\":true},\"alarms\":[],\"reading\":{}}}");
    App setup(s3, clock, wifi, pc, disp, &st3, nullptr);
    setup.boot();
    setup.host_goto(ScreenId::OnboardingCompanionQr);
    setup.handle(InputEvent::Select);  // "Try again" → gets a pairing code
    clock.t += 3000;
    setup.tick(clock.t);  // poll → claimed
    CHECK(setup.screen() == ScreenId::OnboardingRestore);
    setup.handle(InputEvent::Select);  // Restore
    CHECK(setup.screen() == ScreenId::OnboardingPinLength);
    CHECK(setup.config().quiet_enabled);
  }

  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::puts("test_features_23 OK");
  return 0;
}
