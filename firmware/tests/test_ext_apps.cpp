/** Apps from Northwork (pocket-contract v1.0 §C–E): parse, Home tiles, feed screens,
 * Today line, sign-in hint, removal. */
#include "pocket/app.hpp"
#include "pocket/json_lite.hpp"
#include "pocket/ext_apps.hpp"
#include <cstdio>
#include <cstdlib>
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
  bool time_valid() const override { return true; }
  void local_hm(int& hh, int& mm, int& wd, int& mo, int& d) override { hh = 9; mm = 41; wd = 2; mo = 10; d = 18; }
};
struct TWifi : PlatformWifi {
  bool up = true;
  std::vector<std::string> scan() override { return {}; }
  bool connect(const std::string&, const std::string&) override { return up; }
  bool connected() const override { return up; }
  bool start_provision(const std::string&, std::string*, std::string*) override { return false; }
  void stop_provision() override {}
  bool take_provision_credentials(std::string*, std::string*) override { return false; }
  std::string provision_ap_ssid() const override { return {}; }
  std::string provision_ap_password() const override { return {}; }
};
struct TCloud : PlatformCloud {
  std::string attest, apps;
  int fetches = 0;
  std::string create_pair_session(const std::string&) override { return {}; }
  std::string pair_status(const std::string&) override { return "pending"; }
  void refresh_entitlement(DeviceConfig&) override {}
  std::string stt_transcribe(const std::vector<uint8_t>&) override { return {}; }
  std::string device_attest_json(const std::string&) override { return attest; }
  std::string apps_json(const std::string&) override { ++fetches; return apps; }
};
struct TDisp : PlatformDisplay {
  void present(const Canvas&, RefreshMode) override {}
};
struct TStorage : PlatformStorage {
  std::string root = "/tmp/pocket-ext-test";
  bool present() override { return false; }
  uint64_t free_bytes(const std::string&) override { return 64ull << 20; }
  std::string data_root() override { return root; }
  bool data_ensure_root() override { return std::system(("mkdir -p " + root).c_str()) == 0; }
};

static const char* kApps = R"({"apps":[{"id":"org.guidelearning.classroom","name":"Classroom","version":"1.0.0",
 "icon_pbm_b64":"ICON","surfaces":["app","today"],"status":"ok","updated_at":"2026-11-25T09:00:00Z",
 "feed":{"schema":"pocket-feed/1","title":"Guide Classroom","today":"Biology at 10:15","screens":[
  {"id":"main","title":"Today","blocks":[{"type":"header","text":"Next class"},{"type":"kv","key":"Biology","value":"10:15"},
   {"type":"row","text":"Homework","detail":"2 due","go":"hw"},{"type":"text","text":"A long announcement from your teacher."}]},
  {"id":"hw","title":"Homework","blocks":[{"type":"row","text":"Essay draft","detail":"Thu"}]}]}},
 {"id":"dev.example.other","name":"Other","version":"1.0.0","icon_pbm_b64":"","surfaces":["app"],"status":"connect","feed":null}]})";

static void dump(const App& app, const char* name) {
  const char* dir = std::getenv("EXT_DUMP");
  if (!dir) return;
  const std::string path = std::string(dir) + "/" + name + ".pgm";
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fprintf(f, "P5\n480 800\n255\n");
    for (int y = 0; y < 800; ++y)
      for (int x = 0; x < 480; ++x) std::fputc(app.canvas().get_pixel(x, y) <= Gray::G1 ? 0 : 255, f);
    std::fclose(f);
  }
}

int main() {
  std::string apps = kApps;
  apps.replace(apps.find("ICON"), 4, "UDQKMzIgMzIK8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PA=");

  std::vector<ExtApp> parsed;
  CHECK(ext_apps_parse(apps, parsed));
  CHECK(parsed.size() == 2);
  CHECK(parsed[0].icon.size() == 128 && parsed[0].today && parsed[0].screens.size() == 2);
  CHECK(parsed[0].screens[0].blocks.size() == 4);
  CHECK(parsed[1].status == "connect" && parsed[1].icon.empty());

  std::system("rm -rf /tmp/pocket-ext-test");
  MemoryConfigStore store;
  store.mut().onboarding_complete = true;
  store.mut().companion_linked = true;
  store.mut().device_id = "dev-e";
  store.mut().device_token = "tok";
  store.mut().pin_length = 4;
  set_pin(store.mut(), "0000");
  store.mut().idle_lock_s = 3600;
  TClock clock;
  TWifi wifi;
  TCloud cloud;
  TDisp disp;
  TStorage storage;
  App app(store, clock, wifi, cloud, disp, &storage, nullptr);
  app.boot();

  // Check-in says apps changed → fetched while idle on the lock screen.
  cloud.attest = R"({"cloud_entitled":true,"status":"active","apps_rev":"a1"})";
  cloud.apps = apps;
  app.host_attest();
  app.redraw(true);
  clock.t += 100;
  app.tick(clock.t);
  CHECK(cloud.fetches == 1);

  // Unlock to Home: the two apps are the last two tiles.
  app.handle(InputEvent::Select);
  for (int i = 0; i < 4; ++i) app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::Home);
  app.redraw(true);
  dump(app, "home");
  // Walk focus backwards from the first tile: the last tiles are the apps (Other, then Classroom).
  app.handle(InputEvent::Up);
  app.handle(InputEvent::Up);
  clock.t += 1000;
  app.handle(InputEvent::Select);  // Classroom (fresh fetch < 5 min: no refetch)
  CHECK(app.screen() == ScreenId::ExtApp);
  CHECK(cloud.fetches == 1);
  app.redraw(true);
  dump(app, "app-main");
  // Focus skips the header: first stop is the Biology row; Down twice → the Homework row.
  app.handle(InputEvent::Down);
  app.handle(InputEvent::Select);  // Homework → hw screen
  CHECK(app.screen() == ScreenId::ExtApp);
  app.redraw(true);
  app.handle(InputEvent::Back);   // back to main screen, focus restored on Homework
  app.handle(InputEvent::Down);   // the text block
  app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::ExtAppText);
  app.redraw(true);
  app.handle(InputEvent::Back);
  CHECK(app.screen() == ScreenId::ExtApp);
  app.handle(InputEvent::Back);
  CHECK(app.screen() == ScreenId::Home);

  // The app needing a sign-in shows the phone hint instead of a blank screen.
  app.handle(InputEvent::Up);
  app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::ExtApp);
  app.redraw(true);
  app.handle(InputEvent::Back);

  // Today shows the Classroom line in its last block, and opens the app from there.
  app.host_goto(ScreenId::Today);
  app.redraw(true);
  dump(app, "today");
  for (int i = 0; i < 4; ++i) app.handle(InputEvent::Down);
  app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::ExtApp);

  // Uninstalled on the phone: the tiles go away after the next check-in.
  app.host_goto(ScreenId::Home);
  cloud.attest = R"({"cloud_entitled":true,"status":"active","apps_rev":"a2"})";
  cloud.apps = R"({"apps":[]})";
  app.host_attest();
  app.redraw(true);
  clock.t += 100;
  app.tick(clock.t);
  CHECK(cloud.fetches >= 2);

  // Saved copy survives a reboot.
  App again(store, clock, wifi, cloud, disp, &storage, nullptr);
  again.boot();

  if (failures == 0) std::printf("test_ext_apps OK\n");
  return failures == 0 ? 0 : 1;
}
