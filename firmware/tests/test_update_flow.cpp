/** PocketOS update UX: check → available (name + notes) → install → welcome on next boot → lock → PIN. */
#include "pocket/app.hpp"
#include "pocket/release.hpp"
#include <cstdio>
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
  void local_hm(int& h, int& m, int& wd, int& mo, int& d) override { h = 9; m = 0; wd = 1; mo = 10; d = 16; }
};
struct TWifi : PlatformWifi {
  bool up = true;
  std::vector<std::string> scan() override { return {}; }
  bool connect(const std::string&, const std::string&) override { return true; }
  bool connected() const override { return up; }
  bool start_provision(const std::string&, std::string*, std::string*) override { return false; }
  void stop_provision() override {}
  bool take_provision_credentials(std::string*, std::string*) override { return false; }
  std::string provision_ap_ssid() const override { return {}; }
  std::string provision_ap_password() const override { return {}; }
};
struct TCloud : PlatformCloud {
  std::string manifest;
  std::string create_pair_session(const std::string&) override { return {}; }
  std::string pair_status(const std::string&) override { return "pending"; }
  void refresh_entitlement(DeviceConfig&) override {}
  std::string stt_transcribe(const std::vector<uint8_t>&) override { return {}; }
  std::string firmware_latest_json() override { return manifest; }
};
struct TDisp : PlatformDisplay {
  int presents = 0;
  void present(const Canvas&, RefreshMode) override { ++presents; }
};
struct TOta : PlatformOta {
  std::string url;
  std::vector<int> seen;
  ScreenId screen_during = ScreenId::Home;
  App* app = nullptr;
  bool apply_https_ota(const std::string& u, std::string* st, const std::function<void(int)>& p) override {
    url = u;
    if (app) screen_during = app->screen();
    for (int i = 0; i <= 100; i += 10) { seen.push_back(i); if (p) p(i); }
    if (st) *st = "Couldn't verify the update.";
    return false;  // real device restarts; a return means failure
  }
};

int main() {
  const char* kManifest =
      R"({"build_id":"POCKET-LIVE-v999-next","version":"9.9.0","url":"https://x/fw.bin","size":1500000,)"
      R"("version_name":"PocketOS 27.1 Beta","notes":["Faster PIN","New screensavers"]})";

  // --- Check & install ------------------------------------------------------
  {
    MemoryConfigStore store;
    store.mut().onboarding_complete = true;
    store.mut().pin_length = 4;
    set_pin(store.mut(), "0000");
    TClock clock; TWifi wifi; TCloud cloud; TDisp disp; TOta ota;
    App app(store, clock, wifi, cloud, disp, nullptr, nullptr, nullptr, &ota);
    ota.app = &app;
    app.set_build_id("POCKET-LIVE-v104-pocketos21");
    app.boot();
    CHECK(!app.showing_update_welcome());  // first boot ever: no welcome

    // Up to date.
    cloud.manifest = R"({"build_id":"POCKET-LIVE-v104-pocketos21","url":"https://x/fw.bin"})";
    app.host_goto(ScreenId::SettingsUpdate);
    app.handle(InputEvent::Select);
    CHECK(app.screen() == ScreenId::SettingsUpdateResult);
    app.handle(InputEvent::Select);
    CHECK(app.screen() == ScreenId::SettingsUpdate);

    // Offline.
    wifi.up = false;
    app.handle(InputEvent::Select);
    CHECK(app.screen() == ScreenId::SettingsUpdateResult);
    app.handle(InputEvent::Back);
    wifi.up = true;

    // Update available → show it, nothing downloaded yet.
    cloud.manifest = kManifest;
    app.handle(InputEvent::Select);
    CHECK(app.screen() == ScreenId::SettingsUpdateAvailable);
    CHECK(ota.url.empty());
    // Later returns without installing.
    app.handle(InputEvent::Down);
    app.handle(InputEvent::Select);
    CHECK(app.screen() == ScreenId::SettingsUpdate);
    CHECK(ota.url.empty());

    // Install now.
    app.handle(InputEvent::Select);
    CHECK(app.screen() == ScreenId::SettingsUpdateAvailable);
    app.handle(InputEvent::Select);
    CHECK(ota.url == "https://x/fw.bin");
    CHECK(ota.screen_during == ScreenId::SettingsUpdateProgress);
    CHECK(!ota.seen.empty());
    // Failure surfaces as a result, not a hang.
    CHECK(app.screen() == ScreenId::SettingsUpdateResult);
  }

  // --- Next boot on a new build: welcome → lock → PIN ------------------------
  {
    MemoryConfigStore store;
    store.mut().onboarding_complete = true;
    store.mut().pin_length = 4;
    set_pin(store.mut(), "0000");
    store.mut().fw_build_id = "POCKET-LIVE-v103-reliable";
    TClock clock; TWifi wifi; TCloud cloud; TDisp disp;
    App app(store, clock, wifi, cloud, disp);
    app.set_build_id("POCKET-LIVE-v104-pocketos21");
    app.boot();
    app.tick(clock.t);
    CHECK(app.screen() == ScreenId::Lock);
    CHECK(app.showing_update_welcome());
    app.handle(InputEvent::Select);  // dismiss welcome
    CHECK(app.screen() == ScreenId::Lock);
    CHECK(!app.showing_update_welcome());
    app.handle(InputEvent::Select);  // now PIN entry
    CHECK(app.screen() == ScreenId::Pin);

    // Rebooting the same build again: no welcome.
    App again(store, clock, wifi, cloud, disp);
    again.set_build_id("POCKET-LIVE-v104-pocketos21");
    again.boot();
    CHECK(!again.showing_update_welcome());
  }

  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("test_update_flow: OK\n");
  return 0;
}
