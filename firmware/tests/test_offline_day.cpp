/** A day without Wi-Fi: saved weather says when it was last updated, updates ask for the
 * internet, and Pocket never auto-locks while you're using it (dial on Home, reading). */
#include "pocket/app.hpp"
#include <cstdio>
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
  uint32_t now_ms() override { return t; }
  void local_hm(int& h, int& m, int& wd, int& mo, int& d) override { h = 10; m = 0; wd = 1; mo = 3; d = 24; }
};
struct TWifi : PlatformWifi {
  std::vector<std::string> scan() override { return {}; }
  bool connect(const std::string&, const std::string&) override { return false; }
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

static void pump(App& app, TClock& c, uint32_t ms) {
  for (uint32_t d = 0; d < ms; d += 100) {
    c.t += 100;
    app.tick(c.t);
    app.flush_dirty();
  }
}

int main() {
  MemoryConfigStore store;
  store.mut().onboarding_complete = true;
  store.mut().pin_length = 4;
  set_pin(store.mut(), "0000");
  store.mut().idle_lock_s = 60;
  TClock clock; TWifi wifi; TCloud cloud; TDisp disp;
  App app(store, clock, wifi, cloud, disp);
  app.boot();
  app.handle(InputEvent::Select);
  for (int i = 0; i < 4; ++i) app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::Home);

  // Weather: saved forecast says when it was last updated.
  auto& w = app.host_data().weather;
  w.days = {{"2026-11-16", 20, 13, "Clear"}};
  w.fetched_at = static_cast<int64_t>(std::time(nullptr)) - 26 * 3600;
  CHECK(app.host_weather_label().rfind("Last updated yesterday at ", 0) == 0 ||
        app.host_weather_label().rfind("Last updated today at ", 0) == 0);
  w.fetched_at = static_cast<int64_t>(std::time(nullptr)) - 10 * 24 * 3600;
  CHECK(app.host_weather_label().rfind("Last updated ", 0) == 0);
  CHECK(app.host_weather_label().find(" at ") == std::string::npos);  // "Last updated Nov 7"

  // Updates need the internet — and say so.
  app.host_goto(ScreenId::SettingsUpdate);
  app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::SettingsUpdateResult);
  app.handle(InputEvent::Back);

  // Turning the dial on Home for two minutes is "using it": no auto-lock.
  app.host_goto(ScreenId::Home);
  for (int i = 0; i < 24; ++i) {
    clock.t += 5000;
    app.apply_coalesced_rotary(1);
    app.tick(clock.t);
  }
  CHECK(app.screen() == ScreenId::Home);
  // Truly idle → locks after the delay.
  pump(app, clock, 61000);
  CHECK(app.screen() == ScreenId::Lock);

  // Reading a page: 3 minutes without a press still doesn't lock.
  app.handle(InputEvent::Select);
  for (int i = 0; i < 4; ++i) app.handle(InputEvent::Select);
  app.host_goto(ScreenId::ReadingBook);
  app.host_open_book(std::string(4000, 'a'), 0);
  pump(app, clock, 180000);
  CHECK(app.screen() == ScreenId::ReadingBook);

  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::puts("test_offline_day OK");
  return 0;
}
