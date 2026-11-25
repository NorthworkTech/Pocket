#pragma once
/**
 * Apps from Northwork (pocket-contract v1.0 §C–E). Pocket runs no app code: Pocket Cloud
 * fetches each installed app's feed (pocket-feed/1), validates it, and hands this Pocket a
 * read-only description built from a fixed set of blocks that the UI kit draws.
 */
#include "pocket/canvas.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pocket {

inline constexpr int kExtAppMax = 6;  // Home's free tiles (HomeApp::Slot10..Slot15)

struct ExtBlock {
  enum class Kind : uint8_t { Header, Kv, Row, Text, Empty };
  Kind kind = Kind::Row;
  std::string text;    // header / row / text / empty; kv key
  std::string value;   // kv value / row detail
  std::string go;      // row → screen id
};

struct ExtScreen {
  std::string id;
  std::string title;
  std::vector<ExtBlock> blocks;  // dividers dropped
};

struct ExtApp {
  std::string id;
  std::string name;
  std::string status;  // ok | connect | reconnect | waiting
  bool today = false;  // shows a line on Today
  std::vector<uint8_t> icon;  // 32×32 1-bit raster (128 bytes), MSB-first; empty = none
  std::string feed_title;
  std::string today_line;
  std::vector<ExtScreen> screens;
  std::string updated_at;
};

/** Parse Pocket Cloud's {"apps":[…]} (also our saved copy). Keeps at most kExtAppMax. */
bool ext_apps_parse(const std::string& json, std::vector<ExtApp>& out);

/** Draw a 32×32 app icon scaled to `size` (nearest neighbour), centered on (cx, cy). */
void ext_app_icon(Canvas& c, const ExtApp& app, int cx, int cy, int size, Gray ink);

const ExtScreen* ext_find_screen(const ExtApp& app, const std::string& id);

}  // namespace pocket
