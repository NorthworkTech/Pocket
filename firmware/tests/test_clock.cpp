/** Clock app: create alarm via the dial, alarm fires (even over Lock), snooze, timers. */
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
  int h = 7, m = 58;
  uint32_t now_ms() override { return t; }
  void local_hm(int& hh, int& mm, int& wd, int& mo, int& d) override {
    hh = h;
    mm = m;
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
struct TAudio : PlatformAudio {
  int beeps = 0;
  void play(SoundId id) override {
    if (id == SoundId::Attention) ++beeps;
  }
};

static void advance(App& app, TClock& c, uint32_t ms) {
  for (uint32_t done = 0; done < ms; done += 100) {
    c.t += 100;
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
  TAudio audio;
  App app(store, clock, wifi, cloud, disp, nullptr, &audio);
  app.boot();
  app.host_goto(ScreenId::ClockFace);
  app.tick(clock.t);

  // Clock face → Alarms tab → open Alarms → New alarm.
  app.handle(InputEvent::Down);
  app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::ClockAlarms);
  app.handle(InputEvent::Select);  // "New alarm" (index 0 with no alarms)
  CHECK(app.screen() == ScreenId::ClockAlarmEdit);
  // Default 7:00 → adjust hour +1 → 8:00.
  app.handle(InputEvent::Select);  // Hour row → adjust mode
  app.handle(InputEvent::Down);    // +1 hour
  app.handle(InputEvent::Select);  // done adjusting
  for (int i = 0; i < 3; ++i) app.handle(InputEvent::Down);  // → Save
  app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::ClockAlarms);
  CHECK(app.host_data().alarms.size() == 1);
  CHECK(!app.host_data().alarms.empty() && app.host_data().alarms[0].minutes_of_day == 8 * 60);
  CHECK(!app.host_data().alarms.empty() && app.host_data().alarms[0].enabled);

  // Lock the device; the alarm must still ring at 8:00 — over the lock screen.
  app.handle(InputEvent::PowerLong);
  CHECK(app.screen() == ScreenId::Lock);
  clock.m = 59;
  advance(app, clock, 500);
  CHECK(app.screen() == ScreenId::Lock);
  clock.h = 8;
  clock.m = 0;
  advance(app, clock, 500);
  CHECK(app.screen() == ScreenId::ClockRinging);
  advance(app, clock, 6000);
  CHECK(audio.beeps >= 2);

  // Snooze (row 1) → back to Lock; rings again 9 minutes later.
  app.handle(InputEvent::Down);
  app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::Lock);
  clock.m = 5;
  advance(app, clock, 500);
  CHECK(app.screen() == ScreenId::Lock);
  clock.m = 9;
  advance(app, clock, 500);
  CHECK(app.screen() == ScreenId::ClockRinging);
  app.handle(InputEvent::Select);  // Dismiss
  CHECK(app.screen() == ScreenId::Lock);

  // Same minute again does not re-ring.
  advance(app, clock, 2000);
  CHECK(app.screen() == ScreenId::Lock);

  // Timer: 1 minute → Time's up → Dismiss returns to the timer list.
  app.host_goto(ScreenId::ClockTimers);
  app.handle(InputEvent::Select);  // 1 minute
  CHECK(app.screen() == ScreenId::ClockTimerRun);
  advance(app, clock, 30000);
  CHECK(app.screen() == ScreenId::ClockTimerRun);
  // Pause holds the countdown.
  app.handle(InputEvent::Select);
  advance(app, clock, 60000);
  CHECK(app.screen() == ScreenId::ClockTimerRun);
  app.handle(InputEvent::Select);  // resume
  advance(app, clock, 31000);
  CHECK(app.screen() == ScreenId::ClockRinging);
  app.handle(InputEvent::Select);
  CHECK(app.screen() == ScreenId::ClockTimers);

  // Power button: tap → Home from anywhere; hold → Lock; tap on Lock → PIN entry.
  {
    app.host_goto(ScreenId::NotesList);
    app.handle(InputEvent::Power);
    CHECK(app.screen() == ScreenId::Home);
    app.host_goto(ScreenId::SettingsDisplay);
    app.handle(InputEvent::Power);
    CHECK(app.screen() == ScreenId::Home);
    app.handle(InputEvent::PowerLong);
    CHECK(app.screen() == ScreenId::Lock);
    app.handle(InputEvent::Power);
    CHECK(app.screen() == ScreenId::Pin);
  }

  if (failures == 0) std::printf("test_clock: OK\n");
  return failures == 0 ? 0 : 1;
}
