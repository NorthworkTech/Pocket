/** Render every major screen (with sample data) to PPM for design QA.
 *  Usage: dump_gallery OUT_DIR   →  OUT_DIR/NN-name.ppm
 *  tools/gallery.py stitches these into a contact sheet. */
#include "pocket/app.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace pocket;

struct TClock : PlatformClock {
  uint32_t now_ms() override { return 1000; }
  void local_hm(int& hour, int& minute, int& weekday, int& month, int& day) override {
    hour = 9;
    minute = 41;
    weekday = 1;
    month = 10;
    day = 16;
  }
  int battery_percent() override { return 78; }
};
struct TWifi : PlatformWifi {
  std::vector<std::string> scan() override { return {"Home", "Cafe"}; }
  bool connect(const std::string&, const std::string&) override { return true; }
  bool connected() const override { return true; }
  bool start_provision(const std::string&, std::string* ap, std::string* pw) override {
    if (ap) *ap = "Pocket-7F2A";
    if (pw) *pw = "AB23CD45";
    return true;
  }
  void stop_provision() override {}
  bool take_provision_credentials(std::string*, std::string*) override { return false; }
  std::string provision_ap_ssid() const override { return "Pocket-7F2A"; }
  std::string provision_ap_password() const override { return "AB23CD45"; }
};
struct TCloud : PlatformCloud {
  std::string create_pair_session(const std::string&) override { return "K7Q2M9"; }
  std::string pair_status(const std::string&) override { return "pending"; }
  void refresh_entitlement(DeviceConfig&) override {}
  std::string stt_transcribe(const std::vector<uint8_t>&) override { return {}; }
};
struct TDisp : PlatformDisplay {
  void present(const Canvas&, RefreshMode) override {}
};

static bool write_ppm(const std::string& path, const Canvas& c) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", kCanvasW, kCanvasH);
  for (int y = 0; y < kCanvasH; ++y)
    for (int x = 0; x < kCanvasW; ++x) {
      const uint8_t v = static_cast<uint8_t>(static_cast<int>(c.get_pixel(x, y)) * 85);
      std::fputc(v, f);
      std::fputc(v, f);
      std::fputc(v, f);
    }
  std::fclose(f);
  return true;
}

int main(int argc, char** argv) {
  const std::string out = (argc > 1) ? argv[1] : ".";
  MemoryConfigStore store;
  store.mut().onboarding_complete = true;
  store.mut().pin_length = 4;
  set_pin(store.mut(), "0000");
  store.mut().home_visible = 0x01FF;
  store.mut().device_name = "Pocket";
  TClock clock;
  TWifi wifi;
  TCloud cloud;
  TDisp disp;
  App app(store, clock, wifi, cloud, disp);
  app.boot();

  auto& d = app.host_data();
  d.notes = {{"n1", "Grocery ideas", "Oat milk, lemons, basil, the good bread from the corner.", 0, 0, {}},
             {"n2", "Call Sam after lunch", "About the cabin weekend.", 0, 0, {}},
             {"n3", "Weekend packing", "Boots, rain shell, book.", 0, 0, {}}};
  TodoList l;
  l.id = "l1";
  l.title = "Groceries";
  l.items = {{"i1", "Oat milk", true, 0, {}}, {"i2", "Lemons", false, 0, {}}, {"i3", "Basil", false, 0, {}}};
  d.lists = {l};
  d.alarms = {{"a1", 7 * 60, true, "Wake"}, {"a2", 22 * 60 + 30, false, "Wind down"}};
  d.weather.city = "Lisbon";
  d.weather.fetched_at = 1;
  d.weather.today_temp = 19;
  d.weather.today_condition = "Partly cloudy";
  d.weather.days = {{"2026-11-16", 20, 13, "Partly cloudy"}, {"2026-11-17", 18, 12, "Rain"},
                    {"2026-11-18", 21, 14, "Clear"}};
  app.host_tracks() = {{"t1", "Clair de Lune", "clair.mp3", "", 1}, {"t2", "Gymnopédie No.1", "g.mp3", "", 1}};
  app.host_books() = {{"b1", "Walden", "H. D. Thoreau", "epub", "w.txt", "", 1}};

  // Every screen, in ScreenId order (names mirror nav.hpp).
  static const char* kNames[] = {
      "lock", "pin", "ob-welcome", "ob-get-app", "ob-sd", "ob-wifi-list", "ob-softap", "ob-connecting",
      "ob-pair-qr", "ob-pin-length", "ob-pin-set", "ob-pin-confirm", "ob-timezone", "ob-mic", "ob-done",
      "sd-gate", "home", "notes", "note", "lists", "list", "ledger", "clock", "alarms", "alarm-edit",
      "timers", "timer-run", "pass", "pass-detail", "weather", "weather-city", "music", "now-playing",
      "reading", "book", "settings", "security", "wifi", "display", "sound", "home-apps", "units", "about",
      "cloud", "update", "update-progress", "update-result"};
  const int n_screens = static_cast<int>(ScreenId::SettingsUpdateResult) + 1;
  int i = 0;
  for (int sidx = 0; sidx < n_screens; ++sidx) {
    const ScreenId id = static_cast<ScreenId>(sidx);
    if (id == ScreenId::OnboardingWifiConnecting) continue;
    int tab = 0;
    if (id == ScreenId::ListsList || id == ScreenId::ListsDetail) tab = 1;
    if (id == ScreenId::ClockAlarms || id == ScreenId::ClockAlarmEdit) tab = 1;
    if (id == ScreenId::ClockTimers || id == ScreenId::ClockTimerRun) tab = 2;
    app.host_goto(id, tab);
    app.redraw(true);
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%02d-%s.ppm", out.c_str(), i++, kNames[sidx]);
    if (!write_ppm(path, app.canvas())) return 1;
  }
  {
    std::string text;
    for (int k = 0; k < 6; ++k)
      text += "When I wrote the following pages, or rather the bulk of them, I lived alone, in the woods, a mile "
              "from any neighbor, in a house which I had built myself, on the shore of Walden Pond, and earned my "
              "living by the labor of my hands only. I lived there two years and two months.\n\n";
    app.host_goto(ScreenId::ReadingBook);
    app.host_open_book(text, 2);
    app.redraw(true);
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%02d-book-page.ppm", out.c_str(), i++);
    if (!write_ppm(path, app.canvas())) return 1;
  }
  for (int m = 1; m < kLockMotifCount; ++m) {
    app.handle(InputEvent::Power);
    app.redraw(true);
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%02d-lock-motif%d.ppm", out.c_str(), i++, app.lock_motif_index());
    if (!write_ppm(path, app.canvas())) return 1;
  }
  std::printf("wrote %d screens to %s\n", i, out.c_str());
  return 0;
}
