/** SD reformat says "Reformatting…" (painted before the blocking erase). Based on setup fakes.
 * Setup: phone sends Wi-Fi → Pocket joins → Link. A flaky/unreachable Pocket Cloud never sends
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


struct TDisp2 : PlatformDisplay {
  int presents = 0;
  bool saw_formatting = false;
  void present(const Canvas&, RefreshMode) override { ++presents; }
  void present_region(const Canvas&, int, int, int, int) override { ++presents; }
};
struct TStorage : PlatformStorage {
  App** app = nullptr;
  TDisp2* disp = nullptr;
  int presents_at_erase = -1;
  bool present() override { return true; }
  bool probe() override { return true; }
  SdContentKind classify() override { return SdContentKind::Media; }
  bool erase_card() override {
    presents_at_erase = disp->presents;
    return true;
  }
};

int main() {
  MemoryConfigStore store;
  TClock clock; TWifi wifi; TCloud cloud; TDisp2 disp; TStorage storage;
  storage.disp = &disp;
  App app(store, clock, wifi, cloud, disp, &storage);
  app.boot();
  app.host_goto(ScreenId::OnboardingWelcome);
  app.handle(InputEvent::Select);  // → Storage step, card detected
  CHECK(app.screen() == ScreenId::OnboardingSdCard);
  const int before = disp.presents;
  app.handle(InputEvent::Select);  // Reformat and continue
  CHECK(storage.presents_at_erase > before);  // "Reformatting…" was on screen before the erase
  CHECK(app.screen() != ScreenId::OnboardingSdCard);  // and setup moved on afterwards
  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::puts("test_sd_reformat OK");
  return 0;
}
