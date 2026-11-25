#pragma once
/**
 * Wallpapers from Northwork (pocket-contract v1.0 §B): 480×800 1-bit PBM (P4, 1 = black,
 * rows MSB-first) plus a layout with a live clock box and an optional info line.
 * Stored under <data_root>/wallpapers: <id>.pbm per image and index.json for the list.
 */
#include "pocket/canvas.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pocket {

inline constexpr int kWallW = 480;
inline constexpr int kWallH = 800;
inline constexpr size_t kWallRasterBytes = (kWallW / 8) * kWallH;  // 48,000
inline constexpr size_t kWallMaxFileBytes = 64 * 1024;
inline constexpr int kWallMax = 5;
/** DeviceConfig::screensaver value: show the selected Northwork wallpaper. */
inline constexpr uint8_t kScreensaverWallpaper = 254;

struct WallpaperItem {
  std::string id;
  std::string name;
  std::string sha256;
  bool clock = false;
  int clock_x = 0, clock_y = 0, clock_w = 0, clock_h = 0;
  bool clock_large = true;
  bool clock_white = false;
  bool info = false;
  bool info_white = false;
};

struct WallpaperIndex {
  std::string rev;       // attest wallpapers_rev this list matches
  std::string selected;  // id shown when screensaver == kScreensaverWallpaper
  std::vector<WallpaperItem> items;  // newest first, ≤ kWallMax
};

/** Validate a P4 file and return its 48,000-byte raster. */
bool wallpaper_raster_from_pbm(const std::string& file, std::vector<uint8_t>& raster);

/** Paint a raster full-screen (1 → G0 black, 0 → G3 white). */
void wallpaper_blit(Canvas& c, const uint8_t* raster);

/** Parse {"wallpapers":[...]} from Pocket Cloud, or our index.json ({rev,selected,wallpapers}). */
bool wallpaper_parse_index(const std::string& json, WallpaperIndex& out);
std::string wallpaper_index_json(const WallpaperIndex& idx);

/** Draw the live clock ("9:41" / "21:41", with AM/PM for 12 h when it fits) into the box. */
void wallpaper_draw_clock(Canvas& c, const WallpaperItem& w, int hour, int minute, bool h24);

}  // namespace pocket
