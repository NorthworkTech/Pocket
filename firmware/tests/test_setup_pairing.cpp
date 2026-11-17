/** Setup: phone sends Wi-Fi → Pocket joins → Link. A flaky/unreachable Pocket Cloud never sends
 * the user back to the (closed) setup network; Link says why and Try again works. Drawing
 * never makes network calls. */
#include "pocket/app.hpp"
#include <cstdio>
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
  int slept = 0;
  uint32_t now_ms() override { return t; }
  void local_hm(int& h, int& m, int& wd, int& mo, int& d) override { h = 10; m = 0; wd = 1; mo = 3; d = 24; }
  void sleep_ms(uint32_t ms) override { slept += static_cast<int>(ms); }
};
struct TWifi : PlatformWifi {
  bool online = false, creds = false, join_ok = true;
  std::vector<std::string> scan() override { return {"Home"}; }
  bool connect(const std::string&, const std::string&) override { online = join_ok; return join_ok; }
  bool connected() const override { return online; }
  bool start_provision(const std::string&, std::string* a, std::string* p) override {
    if (a) *a = "Pocket-TEST";
    if (p) *p = "AB23CD45";
    return true;
  }
  void stop_provision() override {}
  bool take_provision_credentials(std::string* s, std::string* p) override {
    if (!creds) return false;
    creds = false;
    *s = "Home";
    *p = "secret";
    return true;
  }
  std::string provision_ap_ssid() const override { return "Pocket-TEST"; }
  std::string provision_ap_password() const override { return "AB23CD45"; }
  std::string last_error() const override { return "That password didn't work. Check it and try again."; }
};
struct TCloud : PlatformCloud {
  int calls = 0, fail_first = 0;
  std::string create_pair_session(const std::string&) override {
    ++calls;
    return calls <= fail_first ? std::string() : std::string("ABCD2345");
  }
  std::string pair_status(const std::string&) override { return "pending"; }
  void refresh_entitlement(DeviceConfig&) override {}
  std::string stt_transcribe(const std::vector<uint8_t>&) override { return {}; }
  std::string last_error() const override { return "Pocket is on Wi-Fi but can't reach the internet yet."; }
};
struct TDisp : PlatformDisplay {
  void present(const Canvas&, RefreshMode) override {}
};

static void pump(App& app, TClock& c, int ms) {
  for (int d = 0; d < ms; d += 100) {
    c.t += 100;
    app.tick(c.t);
    app.flush_dirty();
  }
}

int main() {
  // 1) Cloud flaky right after joining: retries inside one attempt and lands on a code.
  {
    MemoryConfigStore store;
    TClock clock; TWifi wifi; TCloud cloud; TDisp disp;
    cloud.fail_first = 2;
    App app(store, clock, wifi, cloud, disp);
    app.boot();
    app.host_goto(ScreenId::OnboardingWifiPassword);
    wifi.creds = true;
    pump(app, clock, 1000);
    CHECK(app.screen() == ScreenId::OnboardingCompanionQr);
    CHECK(cloud.calls == 3);
    CHECK(clock.slept > 0);  // backed off between attempts
    const int calls = cloud.calls;
    for (int i = 0; i < 5; ++i) app.redraw(true);
    CHECK(cloud.calls == calls);  // drawing never hits the network
  }
  // 2) Cloud unreachable: Link shows the reason; Try again succeeds once it's back.
  {
    MemoryConfigStore store;
    TClock clock; TWifi wifi; TCloud cloud; TDisp disp;
    cloud.fail_first = 1000;
    App app(store, clock, wifi, cloud, disp);
    app.boot();
    app.host_goto(ScreenId::OnboardingWifiPassword);
    wifi.creds = true;
    pump(app, clock, 1000);
    CHECK(app.screen() == ScreenId::OnboardingCompanionQr);  // not back to the setup network
    const int calls = cloud.calls;
    for (int i = 0; i < 5; ++i) app.redraw(true);
    CHECK(cloud.calls == calls);
    cloud.fail_first = 0;
    app.handle(InputEvent::Select);  // Try again
    CHECK(app.screen() == ScreenId::OnboardingCompanionQr);
    CHECK(cloud.calls == calls + 1);
    // "Choose a different Wi-Fi" from the error state.
    cloud.fail_first = 100000;
    App app2(store, clock, wifi, cloud, disp);
    app2.boot();
    app2.host_goto(ScreenId::OnboardingWifiPassword);
    wifi.creds = true;
    pump(app2, clock, 1000);
    app2.handle(InputEvent::Down);
    app2.handle(InputEvent::Select);
    CHECK(app2.screen() == ScreenId::OnboardingWifiList);
  }
  // 3) Wrong password: stays on the setup network screen with the reason.
  {
    MemoryConfigStore store;
    TClock clock; TWifi wifi; TCloud cloud; TDisp disp;
    wifi.join_ok = false;
    App app(store, clock, wifi, cloud, disp);
    app.boot();
    app.host_goto(ScreenId::OnboardingWifiPassword);
    wifi.creds = true;
    pump(app, clock, 1000);
    CHECK(app.screen() == ScreenId::OnboardingWifiPassword);
    CHECK(cloud.calls == 0);
  }
  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::puts("test_setup_pairing OK");
  return 0;
}
