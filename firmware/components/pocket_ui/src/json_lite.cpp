#include "pocket/json_lite.hpp"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace pocket {

namespace {

constexpr int kMaxDepth = 32;

struct Parser {
  std::string_view s;
  size_t i = 0;

  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }

  bool lit(std::string_view w) {
    if (s.substr(i, w.size()) != w) return false;
    i += w.size();
    return true;
  }

  static void put_utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) {
      o.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      o.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      o.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      o.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      o.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool hex4(uint32_t& v) {
    if (i + 4 > s.size()) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) {
      const char h = s[i++];
      v <<= 4;
      if (h >= '0' && h <= '9') v |= static_cast<uint32_t>(h - '0');
      else if (h >= 'a' && h <= 'f') v |= static_cast<uint32_t>(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') v |= static_cast<uint32_t>(h - 'A' + 10);
      else return false;
    }
    return true;
  }

  bool string(std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
      const char c = s[i++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (i >= s.size()) return false;
      const char e = s[i++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && s.substr(i, 2) == "\\u") {
            const size_t save = i;
            i += 2;
            uint32_t lo = 0;
            if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              i = save;
            }
          }
          put_utf8(out, cp);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool value(JsonValue& v, int depth) {
    if (depth > kMaxDepth) return false;
    ws();
    if (i >= s.size()) return false;
    const char c = s[i];
    if (c == '{') {
      ++i;
      v.type = JsonValue::Type::Object;
      ws();
      if (i < s.size() && s[i] == '}') {
        ++i;
        return true;
      }
      while (true) {
        ws();
        std::string key;
        if (!string(key)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        JsonValue child;
        if (!value(child, depth + 1)) return false;
        v.members.emplace_back(std::move(key), std::move(child));
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == '}') {
          ++i;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      ++i;
      v.type = JsonValue::Type::Array;
      ws();
      if (i < s.size() && s[i] == ']') {
        ++i;
        return true;
      }
      while (true) {
        JsonValue child;
        if (!value(child, depth + 1)) return false;
        v.items.push_back(std::move(child));
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == ']') {
          ++i;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      v.type = JsonValue::Type::String;
      return string(v.str);
    }
    if (lit("true")) {
      v.type = JsonValue::Type::Bool;
      v.boolean = true;
      return true;
    }
    if (lit("false")) {
      v.type = JsonValue::Type::Bool;
      v.boolean = false;
      return true;
    }
    if (lit("null")) {
      v.type = JsonValue::Type::Null;
      return true;
    }
    // Number.
    const size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                            s[i] == '-' || s[i] == '+'))
      ++i;
    if (i == start) return false;
    const std::string num(s.substr(start, i - start));
    char* end = nullptr;
    v.type = JsonValue::Type::Number;
    v.number = std::strtod(num.c_str(), &end);
    return end && *end == '\0';
  }
};

}  // namespace

const JsonValue* JsonValue::get(std::string_view key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& m : members)
    if (m.first == key) return &m.second;
  return nullptr;
}

std::string JsonValue::get_string(std::string_view key, std::string def) const {
  const JsonValue* v = get(key);
  return (v && v->type == Type::String) ? v->str : def;
}

int64_t JsonValue::get_int(std::string_view key, int64_t def) const {
  const JsonValue* v = get(key);
  return (v && v->type == Type::Number) ? static_cast<int64_t>(v->number) : def;
}

double JsonValue::get_number(std::string_view key, double def) const {
  const JsonValue* v = get(key);
  return (v && v->type == Type::Number) ? v->number : def;
}

bool JsonValue::get_bool(std::string_view key, bool def) const {
  const JsonValue* v = get(key);
  return (v && v->type == Type::Bool) ? v->boolean : def;
}

bool json_parse(std::string_view text, JsonValue& out) {
  Parser p{text, 0};
  JsonValue v;
  if (!p.value(v, 0)) {
    out = JsonValue{};
    return false;
  }
  p.ws();
  if (p.i != text.size()) {
    out = JsonValue{};
    return false;
  }
  out = std::move(v);
  return true;
}

std::string json_escape_str(std::string_view s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o.push_back(static_cast<char>(c));
        }
    }
  }
  return o;
}

bool write_file_atomic(const std::string& path, const std::string& content) {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const size_t n = content.empty() ? 0 : std::fwrite(content.data(), 1, content.size(), f);
  bool ok = n == content.size() && std::fflush(f) == 0;
  if (ok) (void)::fsync(::fileno(f));  // best effort: some VFS drivers make this a no-op
  ok = (std::fclose(f) == 0) && ok;
  if (!ok) {
    std::remove(tmp.c_str());
    return false;
  }
  // FAT/LittleFS rename() won't replace an existing file on every VFS — remove first.
  std::remove(path.c_str());
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool read_file(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[1024];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}

bool read_file_recover(const std::string& path, std::string& out) {
  if (read_file(path, out) && !out.empty()) return true;
  // Power cut between remove() and rename() in write_file_atomic leaves only .tmp.
  if (read_file(path + ".tmp", out) && !out.empty()) {
    std::rename((path + ".tmp").c_str(), path.c_str());
    return true;
  }
  return false;
}

}  // namespace pocket
