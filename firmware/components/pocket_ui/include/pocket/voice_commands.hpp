#pragma once
/** Spoken list commands: "add milk and eggs to groceries", "put batteries on my hardware list". */
#include <string>
#include <vector>

namespace pocket {

struct ListCommand {
  std::string list;                // spoken list name, e.g. "groceries"
  std::vector<std::string> items;  // e.g. {"Milk", "Eggs"}
};

/** True when `transcript` is a list command; fills `out`. Pure — no app state. */
bool parse_list_command(const std::string& transcript, ListCommand& out);

/** Loose list-name match: case-insensitive, ignores "list", "my/the", trailing plural "s". */
bool list_name_matches(const std::string& title, const std::string& spoken);

}  // namespace pocket
