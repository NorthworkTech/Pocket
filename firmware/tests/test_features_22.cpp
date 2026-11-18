/** PocketOS 2.2: quiet hours, Find my Pocket, Companion Home order, lock-screen info,
 * quick capture on the lock face, Today screen. */
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
  int h = 23, m = 15;
  uint32_t now_ms() override { return t; }
  void local_hm(int& hh, int& mm, int& wd, int& mo, int& d) override { hh = h; mm = m; wd = 2; mo = 10; d = 18; }
};
struct TWifi : PlatformWifi {
  bool up = false;
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
  std::string attest;
  std::string create_pair_session(const std::string&) override { return {}; }
  std::string pair_status(const std::string&) override { return "pending"; }
  void refresh_entitlement(DeviceConfig&) override {}
  std::string stt_transcribe(const std::vector<uint8_t>&) override { return {}; }
  std::string device_attest_json(const std::string&) override { return attest; }
};
struct TDisp : PlatformDisplay {
  void present(const Canvas&, RefreshMode) override {}
};
struct TAudio : PlatformAudio {
  int sounds = 0, captures = 0;
  void play(SoundId) override { ++sounds; }
  bool mic_supported() override { return true; }
  void start_capture() override { ++captures; }
  MicCaptureResult stop_capture() override {
    MicCaptureResult r;
    r.ok = true;
    r.level_percent = 30;
    r.pcm.assign(32000, 1);
    return r;
  }
};
struct TStorage : PlatformStorage {
  std::string root = "/tmp/pocket-f22-test";
  bool present() override { return false; }
  uint64_t free_bytes(const std::string&) override { return 64ull << 20; }
  std::string data_root() override { return root; }
  bool data_ensure_root() override { return std::system(("mkdir -p " + root).c_str()) == 0; }
};

int main() {
  std::system("rm -rf /tmp/pocket-f22-test");
  MemoryConfigStore store;
  store.mut().onboarding_complete = true;
  store.mut().companion_linked = true;
  store.mut().device_id = "dev-22";
  store.mut().pin_length = 4;
  set_pin(store.mut(), "0000");
  store.mut().idle_lock_s = 3600;
  TClock clock; TWifi wifi; TCloud cloud; TDisp disp; TAudio audio; TStorage storage;
  App app(store, clock, wifi, cloud, disp, &storage, &audio);
  app.boot();

  // --- Quiet hours: clicks muted 22:00–07:00 (crossing midnight); alarms still ring. ---
  app.mut_config().quiet_enabled = true;
  CHECK(app.host_quiet_now());          // 23:15
  clock.h = 6; clock.m = 59;
  CHECK(app.host_quiet_now());
  clock.h = 7; clock.m = 0;
  CHECK(!app.host_quiet_now());
  clock.h = 23;
  const int before = audio.sounds;
  app.handle(InputEvent::Select);       // Lock → PIN plays nothing in quiet hours
  CHECK(audio.sounds == before);
  app.handle(InputEvent::Back);

  // --- Lock-screen info line: date + next alarm. ---
  app.host_data().alarms = {{"a1", 7 * 60, true, "Wake"}};
  const std::string info = app.host_lock_info();
  CHECK(info.find("Tue, Nov 18") != std::string::npos);
  CHECK(info.find("Alarm 7:00 AM") != std::string::npos);

  // --- Quick capture on the lock face: hold back, speak, release → a voice note. ---
  CHECK(app.screen() == ScreenId::Lock);
  const size_t notes_before = app.host_data().notes.size();
  app.handle(InputEvent::PttStart);
  CHECK(audio.captures == 1);
  app.handle(InputEvent::PttStop);
  CHECK(app.host_data().notes.size() == notes_before + 1);
  CHECK(app.screen() == ScreenId::Lock);  // still locked — nothing revealed
  CHECK(!app.host_data().notes.back().pending_pcm.empty());

  // --- Find my Pocket + Home order arrive with the heartbeat. ---
  wifi.up = true;
  cloud.attest = R"({"cloud_entitled":true,"status":"active","ring":true,"home_order":["reading","notes","clock"]})";
  const int s0 = audio.sounds;
  app.handle(InputEvent::Select);
  for (int i = 0; i < 4; ++i) app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::Home);
  app.host_attest();
  CHECK(audio.sounds >= s0 + 4);  // chimed even though it's quiet hours
  CHECK(app.config().home_order.size() == 3);
  CHECK(app.config().home_order[0] == static_cast<uint8_t>(HomeApp::Reading));
  // First Home tile is now Reading.
  app.host_goto(ScreenId::Home);
  app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::ReadingList);

  // --- Today opens and routes to apps. ---
  app.host_goto(ScreenId::Home);
  app.mut_config().home_order = {static_cast<uint8_t>(HomeApp::Today)};
  app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::Today);
  app.redraw(true);
  app.handle(InputEvent::Down);  // Clock block
  app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::ClockFace);

  // --- Power hold hint never locks by itself. ---
  app.host_goto(ScreenId::Home);
  app.handle(InputEvent::PowerHold);
  CHECK(app.screen() == ScreenId::Home);
  app.handle(InputEvent::PowerLong);
  CHECK(app.screen() == ScreenId::Lock);

  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::puts("test_features_22 OK");
  return 0;
}
