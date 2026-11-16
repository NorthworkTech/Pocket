#pragma once
/**
 * Small, strict JSON reader + safe file writes for on-device data.
 *
 * The firmware used to split JSON with find('{') / find('}') and substring key
 * searches, which lost data whenever text contained braces or quotes. This is a
 * real recursive-descent parser (objects, arrays, strings with full escapes incl.
 * \uXXXX surrogate pairs → UTF-8, numbers, booleans, null) sized for our files.
 */
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pocket {

struct JsonValue {
  enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string str;
  std::vector<JsonValue> items;
  std::vector<std::pair<std::string, JsonValue>> members;

  bool is_object() const { return type == Type::Object; }
  bool is_array() const { return type == Type::Array; }
  /** Member lookup (objects only); nullptr when absent. */
  const JsonValue* get(std::string_view key) const;
  std::string get_string(std::string_view key, std::string def = {}) const;
  int64_t get_int(std::string_view key, int64_t def = 0) const;
  double get_number(std::string_view key, double def = 0) const;
  bool get_bool(std::string_view key, bool def = false) const;
};

/** Parse a complete JSON document. Returns false (and leaves `out` Null) on any error. */
bool json_parse(std::string_view text, JsonValue& out);

/** Escape for a JSON string literal (without the surrounding quotes). */
std::string json_escape_str(std::string_view s);

/**
 * Write `content` to `path` atomically: write path.tmp, flush + fsync, then rename
 * over the original. A power cut leaves either the old file or the new one — never
 * a truncated mix.
 */
bool write_file_atomic(const std::string& path, const std::string& content);

/** Read a whole file; false if it can't be opened. */
bool read_file(const std::string& path, std::string& out);

/** read_file, falling back to path.tmp left by an interrupted write_file_atomic. */
bool read_file_recover(const std::string& path, std::string& out);

}  // namespace pocket
