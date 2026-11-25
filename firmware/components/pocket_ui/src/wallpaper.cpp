#include "pocket/wallpaper.hpp"

#include "pocket/json_lite.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace pocket {

bool wallpaper_raster_from_pbm(const std::string& file, std::vector<uint8_t>& raster) {
  // "P4" <ws> 480 <ws> 800 <one ws> raster
  size_t i = 0;
  if (file.size() < 2 || file[0] != 'P' || file[1] != '4') return false;
  i = 2;
  auto skip_ws = [&]() {
    while (i < file.size() && std::isspace(static_cast<unsigned char>(file[i]))) ++i;
  };
  auto read_int = [&](int& v) {
    skip_ws();
    if (i >= file.size() || !std::isdigit(static_cast<unsigned char>(file[i]))) return false;
    v = 0;
    while (i < file.size() && std::isdigit(static_cast<unsigned char>(file[i])) && v < 100000) v = v * 10 + (file[i++] - '0');
    return true;
  };
  int w = 0, h = 0;
  if (!read_int(w) || !read_int(h) || w != kWallW || h != kWallH) return false;
  if (i >= file.size() || !std::isspace(static_cast<unsigned char>(file[i]))) return false;
  ++i;
  if (file.size() - i != kWallRasterBytes) return false;
  raster.assign(file.begin() + static_cast<std::ptrdiff_t>(i), file.end());
  return true;
}

void wallpaper_blit(Canvas& c, const uint8_t* raster) {
  if (!raster) return;
  constexpr int kRow = kWallW / 8;
  for (int y = 0; y < kWallH; ++y) {
    const uint8_t* row = raster + y * kRow;
    for (int bx = 0; bx < kRow; ++bx) {
      const uint8_t b = row[bx];
      for (int k = 0; k < 8; ++k) {
        c.set_pixel(bx * 8 + k, y, (b & (0x80 >> k)) ? Gray::G0 : Gray::G3);
      }
    }
  }
}

namespace {
bool is_id(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (char ch : s) {
    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) return false;
  }
  return true;
}
}  // namespace

bool wallpaper_parse_index(const std::string& json, WallpaperIndex& out) {
  JsonValue root;
  if (!json_parse(json, root) || !root.is_object()) return false;
  const JsonValue* list = root.get("wallpapers");
  if (!list || !list->is_array()) return false;
  WallpaperIndex idx;
  idx.rev = root.get_string("rev");
  idx.selected = root.get_string("selected");
  for (const auto& v : list->items) {
    if (!v.is_object() || static_cast<int>(idx.items.size()) >= kWallMax) continue;
    WallpaperItem w;
    w.id = v.get_string("id");
    if (!is_id(w.id)) continue;  // ids become file names
    w.name = v.get_string("name").substr(0, 40);
    w.sha256 = v.get_string("sha256");
    if (const JsonValue* lay = v.get("layout"); lay && lay->is_object()) {
      if (const JsonValue* ck = lay->get("clock"); ck && ck->is_object()) {
        w.clock_x = static_cast<int>(ck->get_int("x"));
        w.clock_y = static_cast<int>(ck->get_int("y"));
        w.clock_w = static_cast<int>(ck->get_int("w"));
        w.clock_h = static_cast<int>(ck->get_int("h"));
        w.clock_large = ck->get_string("style", "large") != "small";
        w.clock_white = ck->get_string("ink") == "white";
        w.clock = w.clock_x >= 0 && w.clock_y >= 0 && w.clock_w >= 120 && w.clock_h >= 48 &&
                  w.clock_x + w.clock_w <= kWallW && w.clock_y + w.clock_h <= kWallH;
      }
      if (const JsonValue* inf = lay->get("info"); inf && inf->is_object()) {
        w.info = inf->get_bool("show");
        w.info_white = inf->get_string("ink") == "white";
      }
    }
    idx.items.push_back(std::move(w));
  }
  out = std::move(idx);
  return true;
}

std::string wallpaper_index_json(const WallpaperIndex& idx) {
  std::string o = "{\"rev\":\"" + json_escape_str(idx.rev) + "\",\"selected\":\"" + json_escape_str(idx.selected) +
                  "\",\"wallpapers\":[";
  char buf[200];
  for (size_t i = 0; i < idx.items.size(); ++i) {
    const auto& w = idx.items[i];
    if (i) o += ',';
    o += "{\"id\":\"" + json_escape_str(w.id) + "\",\"name\":\"" + json_escape_str(w.name) + "\",\"sha256\":\"" +
         json_escape_str(w.sha256) + "\",\"layout\":{";
    if (w.clock) {
      std::snprintf(buf, sizeof(buf),
                    "\"clock\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"style\":\"%s\",\"ink\":\"%s\"},", w.clock_x,
                    w.clock_y, w.clock_w, w.clock_h, w.clock_large ? "large" : "small",
                    w.clock_white ? "white" : "black");
      o += buf;
    }
    std::snprintf(buf, sizeof(buf), "\"info\":{\"show\":%s,\"ink\":\"%s\"}}}", w.info ? "true" : "false",
                  w.info_white ? "white" : "black");
    o += buf;
  }
  o += "]}";
  return o;
}

void wallpaper_draw_clock(Canvas& c, const WallpaperItem& w, int hour, int minute, bool h24) {
  if (!w.clock) return;
  char hm[8];
  const char* suf = nullptr;
  if (h24) {
    std::snprintf(hm, sizeof(hm), "%02d:%02d", hour, minute);
  } else {
    const int h12 = hour % 12 == 0 ? 12 : hour % 12;
    std::snprintf(hm, sizeof(hm), "%d:%02d", h12, minute);
    suf = hour < 12 ? "AM" : "PM";
  }
  const Gray ink = w.clock_white ? Gray::G3 : Gray::G0;
  // Largest face that fits the box; "small" starts a size down.
  const Canvas::TextRole large[] = {Canvas::TextRole::HugeClock, Canvas::TextRole::Hero, Canvas::TextRole::PinDigit,
                                    Canvas::TextRole::ScreenTitle};
  const Canvas::TextRole small[] = {Canvas::TextRole::PinDigit, Canvas::TextRole::ScreenTitle,
                                    Canvas::TextRole::BodyStrong};
  const Canvas::TextRole* roles = w.clock_large ? large : small;
  const int n = w.clock_large ? 4 : 3;
  for (int i = 0; i < n; ++i) {
    const auto role = roles[i];
    const int tw = c.text_width(hm, role);
    const int th = c.text_height(role);
    if (tw > w.clock_w || th > w.clock_h) continue;
    const int x = w.clock_x + (w.clock_w - tw) / 2;
    int y = w.clock_y + (w.clock_h - th) / 2;
    // AM/PM under the digits when there's room (caption size).
    const int sh = suf ? c.text_height(Canvas::TextRole::Caption) + 2 : 0;
    if (suf && th + sh <= w.clock_h) y = w.clock_y + (w.clock_h - th - sh) / 2;
    // A 2 px halo in the opposite ink keeps the time readable over any part of a photo.
    const Gray halo = w.clock_white ? Gray::G0 : Gray::G3;
    const bool ampm = suf && th + sh <= w.clock_h;
    static const int kOff[8][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2}, {-2, -2}, {2, -2}, {-2, 2}, {2, 2}};
    for (const auto& o : kOff) {
      c.draw_text(x + o[0], y + o[1], hm, role, halo);
      if (ampm)
        c.draw_text_centered(w.clock_x + w.clock_w / 2 + o[0], y + th + 2 + o[1], suf, Canvas::TextRole::Caption, halo);
    }
    c.draw_text(x, y, hm, role, ink);
    if (ampm) c.draw_text_centered(w.clock_x + w.clock_w / 2, y + th + 2, suf, Canvas::TextRole::Caption, ink);
    return;
  }
}

}  // namespace pocket
