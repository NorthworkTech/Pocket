/** Regression tests for PIN / parental / navigation security bugs found in the 2.0 audit. */
#include "pocket/app.hpp"
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
  uint32_t now_ms() override { return t += 200; }  // every sample advances time
  void local_hm(int& h, int& m, int& wd, int& mo, int& d) override {
    h = 9;
    m = 41;
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

static void enter_pin(App& app, const char* digits) {
  for (const char* p = digits; *p; ++p) {
    for (int i = 0; i < *p - '0'; ++i) app.handle(InputEvent::Down);
    app.handle(InputEvent::Select);
  }
}

struct Rig {
  MemoryConfigStore store;
  TClock clock;
  TWifi wifi;
  TCloud cloud;
  TDisp disp;
  App app{store, clock, wifi, cloud, disp};
  explicit Rig(uint16_t gated = 0) {
    store.mut().onboarding_complete = true;
  store.mut().home_visible &= static_cast<uint16_t>(~(1u << 9));  // classic Home grid (no Today)
    store.mut().pin_length = 4;
    store.mut().parental_pin_gated = gated;
    set_pin(store.mut(), "2468");
    app.boot();
  }
  void unlock() {
    app.handle(InputEvent::Select);
    enter_pin(app, "2468");
  }
};

int main() {
  // 1) Lockout survives leaving the PIN screen.
  {
    Rig r;
    r.app.handle(InputEvent::Select);
    for (int k = 0; k < 5; ++k) enter_pin(r.app, "1111");
    CHECK(r.app.pin_lockout_remaining_s() > 0);
    r.app.handle(InputEvent::Back);
    CHECK(r.app.screen() == ScreenId::Lock);
    r.app.handle(InputEvent::Select);
    enter_pin(r.app, "2468");
    CHECK(r.app.screen() != ScreenId::Home);  // still locked out
    CHECK(r.app.pin_lockout_remaining_s() > 0);
  }

  // 2) Controls tour Back returns to About — never into setup's PIN screens.
  {
    Rig r;
    r.unlock();
    CHECK(r.app.screen() == ScreenId::Home);
    r.app.host_goto(ScreenId::SettingsAbout);
    r.app.handle(InputEvent::Select);  // Controls tips
    CHECK(r.app.screen() == ScreenId::OnboardingDone);
    for (int k = 0; k < 6; ++k) r.app.handle(InputEvent::Back);
    CHECK(r.app.screen() != ScreenId::OnboardingPinSet);
    CHECK(r.app.screen() != ScreenId::OnboardingPinConfirm);
    CHECK(r.app.screen() != ScreenId::OnboardingMicTest);
    CHECK(verify_pin(r.app.config(), "2468"));
  }

  // 3) Change PIN requires the current PIN and returns to Security.
  {
    Rig r;
    r.unlock();
    r.app.host_goto(ScreenId::SettingsSecurity);
    r.app.handle(InputEvent::Select);  // Change PIN
    CHECK(r.app.screen() == ScreenId::Pin);
    enter_pin(r.app, "1111");  // wrong
    CHECK(r.app.screen() == ScreenId::Pin);
    enter_pin(r.app, "2468");
    CHECK(r.app.screen() == ScreenId::OnboardingPinSet);
    enter_pin(r.app, "1357");
    CHECK(r.app.screen() == ScreenId::OnboardingPinConfirm);
    enter_pin(r.app, "1357");
    CHECK(r.app.screen() == ScreenId::SettingsSecurity);
    CHECK(verify_pin(r.app.config(), "1357"));
    CHECK(!verify_pin(r.app.config(), "2468"));
  }

  // 4) Parental gate asks for the PIN even after the device was unlocked.
  {
    Rig r(/*gated=*/1u << static_cast<uint8_t>(HomeApp::Notes));
    r.unlock();
    CHECK(r.app.screen() == ScreenId::Home);
    CHECK(r.app.home_focus_index() == 0);
    r.app.handle(InputEvent::Select);  // Notes
    CHECK(r.app.screen() == ScreenId::Pin);
  }

  // 5) Settings round trips never grow the stack (used to jam after 16).
  {
    Rig r;
    r.unlock();
    r.app.host_goto(ScreenId::SettingsRoot);
    for (int k = 0; k < 30; ++k) {
      r.app.handle(InputEvent::Select);  // open first subpage
      r.app.handle(InputEvent::Back);
    }
    CHECK(r.app.screen() == ScreenId::SettingsRoot);
    r.app.handle(InputEvent::Back);
    CHECK(r.app.screen() == ScreenId::Home);
  }

  if (failures == 0) std::printf("test_security: OK\n");
  return failures == 0 ? 0 : 1;
}
