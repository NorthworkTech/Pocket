/** Screensavers + 1-bit UI: every piece paints real art, deterministically; the
 *  choice persists (config v10, v9 blobs still load); and every screen renders
 *  visible content (guards against a screen silently drawing nothing). */
#include "pocket/app.hpp"
#include "pocket/screensaver.hpp"
#include <cstdio>
#include <cstring>
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
  uint32_t now_ms() override { return 5000; }
  void local_hm(int& h, int& m, int& wd, int& mo, int& d) override {
    h = 9;
    m = 41;
    wd = 1;
    mo = 10;
    d = 16;
  }
};
struct TWifi : PlatformWifi {
  std::vector<std::string> scan() override { return {"Home"}; }
  bool connect(const std::string&, const std::string&) override { return true; }
  bool connected() const override { return true; }
  bool start_provision(const std::string&, std::string* a, std::string* p) override {
    if (a) *a = "Pocket-TEST";
    if (p) *p = "AB23CD45";
    return true;
  }
  void stop_provision() override {}
  bool take_provision_credentials(std::string*, std::string*) override { return false; }
  std::string provision_ap_ssid() const override { return "Pocket-TEST"; }
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

/** Pixels the panel shows as black (EPD threshold: G0/G1 → ink). */
static int ink_in(const Canvas& c, int x, int y, int w, int h) {
  int n = 0;
  for (int yy = y; yy < y + h; ++yy)
    for (int xx = x; xx < x + w; ++xx)
      if (static_cast<int>(c.get_pixel(xx, yy)) < 2) ++n;
  return n;
}

static bool same(const Canvas& a, const Canvas& b) {
  return std::memcmp(a.pixels(), b.pixels(), Canvas::kBufBytes) == 0;
}

int main() {
  // 1) Each piece paints a real composition (not blank, not a solid slab), deterministically.
  static Canvas a, b, c2;
  const int area = kCanvasW * 680;
  for (int i = 0; i < kScreensaverCount; ++i) {
    a.clear();
    b.clear();
    c2.clear();
    draw_screensaver(a, i, 0, 0, kCanvasW, 680, 42);
    draw_screensaver(b, i, 0, 0, kCanvasW, 680, 42);
    draw_screensaver(c2, i, 0, 0, kCanvasW, 680, 43);
    const int ink = ink_in(a, 0, 0, kCanvasW, 680);
    if (ink < area / 50 || ink > area * 49 / 50) std::printf("piece %d (%s) ink=%d\n", i, screensaver_name(i), ink);
    CHECK(ink > area / 50);
    CHECK(ink < area * 49 / 50);
    CHECK(same(a, b));    // same seed → same print
    CHECK(!same(a, c2));  // new lock → fresh variation
    CHECK(std::strlen(screensaver_name(i)) > 0);
    // Art stays inside its rect.
    if (ink_in(a, 0, 680, kCanvasW, kCanvasH - 680) != 0) std::printf("piece %d spills\n", i);
    CHECK(ink_in(a, 0, 680, kCanvasW, kCanvasH - 680) == 0);
  }

  // 2) Config v10 round-trip, and a v9 blob still loads (screensaver defaults to Rotate).
  {
    DeviceConfig cfg;
    cfg.screensaver = 3;
    std::vector<uint8_t> blob;
    CHECK(pack_device_config(cfg, blob));
    DeviceConfig out;
    CHECK(unpack_device_config(blob.data(), blob.size(), out));
    CHECK(out.screensaver == 3);
    cfg.screensaver = kScreensaverPlain;
    CHECK(pack_device_config(cfg, blob));
    CHECK(unpack_device_config(blob.data(), blob.size(), out));
    CHECK(out.screensaver == kScreensaverPlain);
    // Forge v9: version field (LE u16 after magic) = 9, drop the trailing screensaver byte.
    std::vector<uint8_t> v9 = blob;
    v9[4] = 9;
    v9[5] = 0;
    v9.pop_back();
    DeviceConfig old;
    CHECK(unpack_device_config(v9.data(), v9.size(), old));
    CHECK(old.screensaver == 0);
  }

  // 3) App: fixed piece sticks across locks; Plain shows the wordmark without art.
  {
    MemoryConfigStore store;
    store.mut().onboarding_complete = true;
    store.mut().pin_length = 4;
    set_pin(store.mut(), "0000");
    store.mut().screensaver = 4;  // always "Fir & fog" (index 3)
    TClock clock;
    TWifi wifi;
    TCloud cloud;
    TDisp disp;
    App app(store, clock, wifi, cloud, disp);
    app.boot();
    app.handle(InputEvent::Select);
    app.handle(InputEvent::Back);
    for (int i = 0; i < 3; ++i) {
      app.host_goto(ScreenId::Home);
      app.handle(InputEvent::PowerLong);
      CHECK(app.screen() == ScreenId::Lock);
      CHECK(app.lock_motif_index() == 3);
    }
  }
  {
    MemoryConfigStore store;
    store.mut().onboarding_complete = true;
    store.mut().pin_length = 4;
    set_pin(store.mut(), "0000");
    store.mut().screensaver = kScreensaverPlain;
    TClock clock;
    TWifi wifi;
    TCloud cloud;
    TDisp disp;
    App app(store, clock, wifi, cloud, disp);
    app.boot();
    app.host_goto(ScreenId::Lock);
    app.redraw(true);
    const int top = ink_in(app.canvas(), 0, 0, kCanvasW, 300);
    const int mid = ink_in(app.canvas(), 0, 300, kCanvasW, 120);
    CHECK(top == 0);   // no art
    CHECK(mid > 200);  // wordmark
  }

  // 4) Every screen draws visible content below the status bar.
  {
    MemoryConfigStore store;
    store.mut().onboarding_complete = true;
    store.mut().pin_length = 4;
    set_pin(store.mut(), "0000");
    TClock clock;
    TWifi wifi;
    TCloud cloud;
    TDisp disp;
    App app(store, clock, wifi, cloud, disp);
    app.boot();
    for (int s = 0; s <= static_cast<int>(ScreenId::SettingsUpdateResult); ++s) {
      const ScreenId id = static_cast<ScreenId>(s);
      if (id == ScreenId::OnboardingWifiConnecting) continue;  // transient (blocking connect)
      if (id == ScreenId::ListsDetail) continue;  // only reachable with a list selected
      app.host_goto(id);
      app.redraw(true);
      const int ink = ink_in(app.canvas(), 0, kStatusBarH + 4, kCanvasW, kCanvasH - kStatusBarH - 4);
      if (ink < 400) std::printf("screen %d looks blank (ink=%d)\n", s, ink);
      CHECK(ink >= 400);
    }
  }

  if (failures == 0) std::printf("test_screensaver: OK\n");
  return failures == 0 ? 0 : 1;
}
