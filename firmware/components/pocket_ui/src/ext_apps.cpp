#include "pocket/ext_apps.hpp"

#include "pocket/json_lite.hpp"

#include <algorithm>
#include <cctype>

namespace pocket {

namespace {

/** Standard base64 → bytes (ignores whitespace; false on bad input). */
bool b64_decode(const std::string& in, std::string& out) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  out.clear();
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || std::isspace(static_cast<unsigned char>(c))) continue;
    const int v = val(c);
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return true;
}

/** 32×32 P4 → 128-byte raster. */
bool icon_from_pbm(const std::string& file, std::vector<uint8_t>& raster) {
  size_t i = 0;
  if (file.size() < 2 || file[0] != 'P' || file[1] != '4') return false;
  i = 2;
  int dims[2] = {0, 0};
  for (int& d : dims) {
    while (i < file.size() && std::isspace(static_cast<unsigned char>(file[i]))) ++i;
    if (i >= file.size() || !std::isdigit(static_cast<unsigned char>(file[i]))) return false;
    while (i < file.size() && std::isdigit(static_cast<unsigned char>(file[i])) && d < 1000) d = d * 10 + (file[i++] - '0');
  }
  if (dims[0] != 32 || dims[1] != 32 || i >= file.size()) return false;
  ++i;  // single whitespace before the raster
  if (file.size() - i != 128) return false;
  raster.assign(file.begin() + static_cast<std::ptrdiff_t>(i), file.end());
  return true;
}

}  // namespace

bool ext_apps_parse(const std::string& json, std::vector<ExtApp>& out) {
  JsonValue root;
  if (!json_parse(json, root) || !root.is_object()) return false;
  const JsonValue* list = root.get("apps");
  if (!list || !list->is_array()) return false;
  std::vector<ExtApp> apps;
  for (const auto& v : list->items) {
    if (!v.is_object() || static_cast<int>(apps.size()) >= kExtAppMax) continue;
    ExtApp a;
    a.id = v.get_string("id");
    a.name = v.get_string("name").substr(0, 20);
    if (a.id.empty() || a.name.empty()) continue;
    a.status = v.get_string("status", "waiting");
    a.updated_at = v.get_string("updated_at");
    if (const JsonValue* s = v.get("surfaces"); s && s->is_array())
      for (const auto& x : s->items) a.today = a.today || (x.type == JsonValue::Type::String && x.str == "today");
    std::string pbm;
    if (b64_decode(v.get_string("icon_pbm_b64"), pbm)) (void)icon_from_pbm(pbm, a.icon);
    if (const JsonValue* f = v.get("feed"); f && f->is_object()) {
      a.feed_title = f->get_string("title");
      a.today_line = f->get_string("today");
      if (const JsonValue* scs = f->get("screens"); scs && scs->is_array()) {
        for (const auto& sv : scs->items) {
          if (!sv.is_object() || a.screens.size() >= 8) continue;
          ExtScreen sc;
          sc.id = sv.get_string("id");
          sc.title = sv.get_string("title");
          if (const JsonValue* bl = sv.get("blocks"); bl && bl->is_array()) {
            for (const auto& bv : bl->items) {
              if (!bv.is_object() || sc.blocks.size() >= 40) continue;
              const std::string t = bv.get_string("type");
              ExtBlock b;
              if (t == "header") {
                b.kind = ExtBlock::Kind::Header;
                b.text = bv.get_string("text");
              } else if (t == "kv") {
                b.kind = ExtBlock::Kind::Kv;
                b.text = bv.get_string("key");
                b.value = bv.get_string("value");
              } else if (t == "row") {
                b.kind = ExtBlock::Kind::Row;
                b.text = bv.get_string("text");
                b.value = bv.get_string("detail");
                b.go = bv.get_string("go");
              } else if (t == "text") {
                b.kind = ExtBlock::Kind::Text;
                b.text = bv.get_string("text");
              } else if (t == "empty") {
                b.kind = ExtBlock::Kind::Empty;
                b.text = bv.get_string("text");
              } else {
                continue;  // divider and anything newer
              }
              sc.blocks.push_back(std::move(b));
            }
          }
          if (!sc.id.empty()) a.screens.push_back(std::move(sc));
        }
      }
    }
    if (a.status == "ok" && a.screens.empty()) a.status = "waiting";
    apps.push_back(std::move(a));
  }
  out = std::move(apps);
  return true;
}

void ext_app_icon(Canvas& c, const ExtApp& app, int cx, int cy, int size, Gray ink) {
  if (app.icon.size() != 128) {
    // No icon: the app's initial.
    const std::string ch = app.name.substr(0, 1);
    c.draw_text_centered(cx, cy - c.text_height(Canvas::TextRole::ScreenTitle) / 2, ch, Canvas::TextRole::ScreenTitle, ink);
    return;
  }
  const int scale = std::max(1, size / 32);
  const int x0 = cx - 16 * scale, y0 = cy - 16 * scale;
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 32; ++x)
      if (app.icon[static_cast<size_t>(y * 4 + x / 8)] & (0x80 >> (x % 8))) c.fill_rect(x0 + x * scale, y0 + y * scale, scale, scale, ink);
}

const ExtScreen* ext_find_screen(const ExtApp& app, const std::string& id) {
  for (const auto& s : app.screens)
    if (s.id == id) return &s;
  return app.screens.empty() ? nullptr : &app.screens.front();
}

}  // namespace pocket
