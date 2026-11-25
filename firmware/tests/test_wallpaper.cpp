/** Wallpapers from Northwork (pocket-contract v1.0 §B): PBM parsing, index round-trip, and the
 * device flow — check-in rev → list → download → lock face shows it with a live clock. */
#include "pocket/app.hpp"
#include "pocket/json_lite.hpp"
#include "pocket/wallpaper.hpp"
#include <cstdio>
#include <cstdlib>
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

static std::string make_pbm(uint8_t fill) {
  std::string s = "P4\n480 800\n";
  s.append(kWallRasterBytes, static_cast<char>(fill));
  return s;
}

struct TClock : PlatformClock {
  uint32_t t = 1000;
  int h = 9, m = 41;
  uint32_t now_ms() override { return t; }
  bool time_valid() const override { return true; }
  void local_hm(int& hh, int& mm, int& wd, int& mo, int& d) override { hh = h; mm = m; wd = 2; mo = 10; d = 18; }
};
struct TWifi : PlatformWifi {
  bool up = true;
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
  std::string attest, list;
  std::string image = make_pbm(0xFF);  // all black
  int downloads = 0;
  std::string create_pair_session(const std::string&) override { return {}; }
  std::string pair_status(const std::string&) override { return "pending"; }
  void refresh_entitlement(DeviceConfig&) override {}
  std::string stt_transcribe(const std::vector<uint8_t>&) override { return {}; }
  std::string device_attest_json(const std::string&) override { return attest; }
  std::string wallpapers_json(const std::string&) override { return list; }
  bool download_to_file(const char* kind, const std::string&, const std::string&, const std::string& path, int64_t,
                        int64_t* out) override {
    if (std::string(kind) != "wallpaper") return false;
    ++downloads;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(image.data(), 1, image.size(), f);
      std::fclose(f);
      if (out) *out = static_cast<int64_t>(image.size());
      return true;
    }
    return false;
  }
};
struct TDisp : PlatformDisplay {
  void present(const Canvas&, RefreshMode) override {}
};
struct TStorage : PlatformStorage {
  std::string root = "/tmp/pocket-wall-test";
  bool present() override { return false; }
  uint64_t free_bytes(const std::string&) override { return 64ull << 20; }
  std::string data_root() override { return root; }
  bool data_ensure_root() override { return std::system(("mkdir -p " + root).c_str()) == 0; }
};

int main() {
  // --- PBM parsing ---
  std::vector<uint8_t> raster;
  CHECK(wallpaper_raster_from_pbm(make_pbm(0xAA), raster));
  CHECK(raster.size() == kWallRasterBytes && raster[0] == 0xAA);
  CHECK(!wallpaper_raster_from_pbm("P4\n800 480\n" + std::string(kWallRasterBytes, '\0'), raster));
  CHECK(!wallpaper_raster_from_pbm("P4\n480 800\n" + std::string(100, '\0'), raster));
  CHECK(wallpaper_raster_from_pbm("P4 480\t800\n" + std::string(kWallRasterBytes, '\0'), raster));

  // --- Index parse/serialize round-trip; unsafe ids (file names) are dropped ---
  WallpaperIndex idx;
  CHECK(wallpaper_parse_index(
      R"({"wallpapers":[{"id":"wp_a","name":"Fern","sha256":"s1","layout":{"clock":{"x":40,"y":520,"w":400,"h":160,"style":"large","ink":"white"},"info":{"show":true}}},{"id":"../etc","name":"bad"}]})",
      idx));
  CHECK(idx.items.size() == 1);
  CHECK(idx.items[0].clock && idx.items[0].clock_white && idx.items[0].info);
  idx.selected = "wp_a";
  idx.rev = "r1";
  WallpaperIndex back;
  CHECK(wallpaper_parse_index(wallpaper_index_json(idx), back));
  CHECK(back.selected == "wp_a" && back.rev == "r1" && back.items.size() == 1 && back.items[0].clock_y == 520);

  // --- Device flow ---
  std::system("rm -rf /tmp/pocket-wall-test");
  MemoryConfigStore store;
  store.mut().onboarding_complete = true;
  store.mut().companion_linked = true;
  store.mut().device_id = "dev-w";
  store.mut().device_token = "tok";
  store.mut().pin_length = 4;
  set_pin(store.mut(), "0000");
  store.mut().idle_lock_s = 3600;
  TClock clock;
  TWifi wifi;
  TCloud cloud;
  TDisp disp;
  TStorage storage;
  App app(store, clock, wifi, cloud, disp, &storage, nullptr);
  app.boot();
  CHECK(app.screen() == ScreenId::Lock);

  cloud.attest = R"({"cloud_entitled":true,"status":"active","wallpapers_rev":"rev1"})";
  cloud.list =
      R"({"wallpapers":[{"id":"wp_1","name":"Fern at dawn","sha256":"h1","layout":{"clock":{"x":40,"y":520,"w":400,"h":160,"style":"large","ink":"white"},"info":{"show":false}}}]})";
  app.host_attest();
  app.redraw(true);
  clock.t += 100;
  app.tick(clock.t);
  CHECK(cloud.downloads == 1);
  CHECK(app.config().screensaver == kScreensaverWallpaper);  // a new wallpaper shows right away
  app.redraw(true);
  // Black image everywhere except the white clock digits inside the box.
  CHECK(app.canvas().get_pixel(5, 5) == Gray::G0);
  int white_in_box = 0;
  for (int y = 520; y < 680; ++y)
    for (int x = 40; x < 440; ++x) white_in_box += app.canvas().get_pixel(x, y) == Gray::G3;
  CHECK(white_in_box > 200);

  if (const char* dump = std::getenv("WALL_DUMP")) {
    // Preview: a dithered sky with a black-ink clock and the info pill.
    std::string img = "P4\n480 800\n";
    std::string r(kWallRasterBytes, '\0');
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < 800; ++y)
      for (int x = 0; x < 480; ++x) {
        const int dx = x - 240, dy = y - 260;
        int tone = (dx * dx + dy * dy) / 900;  // darker away from the sun
        if (y > 560) tone = 4 + (y - 560) / 30 + ((x / 40 + y / 25) % 3);  // hills
        if (tone > bayer[y % 4][x % 4]) r[y * 60 + x / 8] |= static_cast<char>(0x80 >> (x % 8));
      }
    cloud.image = img + r;
    cloud.attest = R"({"cloud_entitled":true,"status":"active","wallpapers_rev":"revP"})";
    cloud.list = R"({"wallpapers":[{"id":"wp_p","name":"Sunrise","sha256":"hp","layout":{"clock":{"x":40,"y":60,"w":400,"h":200,"style":"large","ink":"black"},"info":{"show":true,"ink":"black"}}},{"id":"wp_1","name":"Fern at dawn","sha256":"h1","layout":{}}]})";
    app.mut_config().lock_message = "Alex";
    app.host_attest();
    app.redraw(true);
    clock.t += 100;
    app.tick(clock.t);
    app.redraw(true);
    if (FILE* f = std::fopen(dump, "wb")) {
      std::fprintf(f, "P5\n480 800\n255\n");
      for (int y = 0; y < 800; ++y)
        for (int x = 0; x < 480; ++x) std::fputc(app.canvas().get_pixel(x, y) == Gray::G0 ? 0 : 255, f);
      std::fclose(f);
    }
    return 0;
  }

  // Same rev again: nothing downloads.
  app.host_attest();
  clock.t += 100;
  app.tick(clock.t);
  CHECK(cloud.downloads == 1);

  // Northwork removes it: the file goes and the lock face falls back to the screensavers.
  cloud.attest = R"({"cloud_entitled":true,"status":"active","wallpapers_rev":"rev2"})";
  cloud.list = R"({"wallpapers":[]})";
  app.host_attest();
  app.redraw(true);
  clock.t += 100;
  app.tick(clock.t);
  CHECK(app.config().screensaver == 0);
  FILE* gone = std::fopen("/tmp/pocket-wall-test/wallpapers/wp_1.pbm", "rb");
  CHECK(gone == nullptr);
  if (gone) std::fclose(gone);

  // Without its own token, a Pocket doesn't ask for wallpapers.
  app.mut_config().device_token.clear();
  cloud.attest = R"({"cloud_entitled":true,"status":"active","wallpapers_rev":"rev3"})";
  cloud.list =
      R"({"wallpapers":[{"id":"wp_2","name":"Two","sha256":"h2","layout":{}}]})";
  app.host_attest();
  app.redraw(true);
  clock.t += 100;
  app.tick(clock.t);
  CHECK(cloud.downloads == 1);

  if (failures == 0) std::printf("test_wallpaper OK\n");
  return failures == 0 ? 0 : 1;
}
