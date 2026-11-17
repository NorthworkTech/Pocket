/** Ghosting: clean full refresh after a pause, never mid-PIN; lock face is always a full refresh. */
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
  uint32_t t = 0;
  uint32_t now_ms() override { return t; }
  void local_hm(int& h, int& m, int& weekday, int& month, int& day) override {
    h = 10;
    m = 5;
    weekday = 1;
    month = 3;
    day = 24;
  }
};
struct TWifi : PlatformWifi {
  std::vector<std::string> scan() override { return {}; }
  bool connect(const std::string&, const std::string&) override { return true; }
  bool connected() const override { return true; }
  bool start_provision(const std::string&, std::string* ap, std::string* pw) override {
    if (ap) *ap = "Pocket-TEST";
    if (pw) *pw = "AB23CD45";
    return true;
  }
  void stop_provision() override {}
  bool take_provision_credentials(std::string*, std::string*) override { return false; }
  std::string provision_ap_ssid() const override { return "Pocket-TEST"; }
  std::string provision_ap_password() const override { return "AB23CD45"; }
};
struct TCloud : PlatformCloud {
  std::string create_pair_session(const std::string&) override { return {}; }
  std::string pair_status(const std::string&) override { return "pending"; }
  void refresh_entitlement(DeviceConfig&) override {}
  std::string stt_transcribe(const std::vector<uint8_t>&) override { return {}; }
};
struct TDisp : PlatformDisplay {
  int full_or_fast = 0;
  int region = 0;
  int last_ry = -1;
  int last_rh = -1;
  void present(const Canvas&, RefreshMode) override { ++full_or_fast; }
  void present_region(const Canvas&, int /*x*/, int y, int /*w*/, int h) override {
    ++region;
    last_ry = y;
    last_rh = h;
  }
};


static void pump(App& app, TClock& c, uint32_t ms) {
  for (uint32_t d = 0; d < ms; d += 100) {
    c.t += 100;
    app.flush_dirty();
    app.tick(c.t);
  }
}

int main() {
  MemoryConfigStore store;
  store.mut().onboarding_complete = true;
  store.mut().pin_length = 4;
  set_pin(store.mut(), "0000");
  store.mut().idle_lock_s = 3600;
  TClock clock;
  TWifi wifi;
  TCloud cloud;
  TDisp disp;
  App app(store, clock, wifi, cloud, disp);
  app.boot();
  app.handle(InputEvent::Select);
  for (int i = 0; i < 4; ++i) app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::Home);
  pump(app, clock, 4000);  // settle (unlock may itself earn a clean)

  // Open an app (whole-screen partial) → after a pause, exactly one clean full refresh.
  int f = disp.full_or_fast;
  app.handle(InputEvent::Select);
  app.flush_dirty();
  CHECK(app.screen() != ScreenId::Home);
  CHECK(disp.full_or_fast == f);  // the screen change itself stays fast
  pump(app, clock, 1000);
  CHECK(disp.full_or_fast == f);  // not while the user is still active
  pump(app, clock, 3000);
  CHECK(disp.full_or_fast == f + 1);
  pump(app, clock, 10000);
  CHECK(disp.full_or_fast == f + 1);  // once, not repeatedly

  // Lock is always a full refresh.
  f = disp.full_or_fast;
  app.handle(InputEvent::PowerLong);
  CHECK(app.screen() == ScreenId::Lock);
  CHECK(disp.full_or_fast == f + 1);

  // PIN entry never gets an idle flash, even with residue.
  app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::Pin);
  app.handle(InputEvent::Down);
  app.handle(InputEvent::Down);
  app.flush_dirty();
  f = disp.full_or_fast;
  pump(app, clock, 8000);
  CHECK(disp.full_or_fast == f);

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::puts("test_ghosting OK");
  return 0;
}
