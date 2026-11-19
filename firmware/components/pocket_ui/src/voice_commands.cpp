#include "pocket/voice_commands.hpp"
#include <cctype>

namespace pocket {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(std::string s) {
  const char* ws = " \t\r\n.,!?;:\"'";
  const size_t a = s.find_first_not_of(ws);
  if (a == std::string::npos) return {};
  const size_t b = s.find_last_not_of(ws);
  return s.substr(a, b - a + 1);
}

/** "my groceries list" → "groceries". */
std::string clean_list_name(std::string s) {
  s = trim(lower(s));
  for (const char* lead : {"my ", "the ", "our ", "a "})
    if (s.rfind(lead, 0) == 0) s = s.substr(std::string(lead).size());
  for (const char* tail : {" list", " lists"}) {
    const std::string t = tail;
    if (s.size() > t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0) s = s.substr(0, s.size() - t.size());
  }
  return trim(s);
}

std::string stem(std::string s) {
  s = clean_list_name(s);
  // groceries → grocery, boxes → box, books → book (but "glass" stays).
  if (s.size() > 4 && s.compare(s.size() - 3, 3, "ies") == 0) return s.substr(0, s.size() - 3) + "y";
  if (s.size() > 4 && (s.compare(s.size() - 3, 3, "xes") == 0 || s.compare(s.size() - 4, 4, "ches") == 0 ||
                       s.compare(s.size() - 4, 4, "shes") == 0))
    return s.substr(0, s.size() - 2);
  if (s.size() > 3 && s.back() == 's' && s[s.size() - 2] != 's') s.pop_back();
  return s;
}

std::string capitalize(std::string s) {
  if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}

}  // namespace

bool list_name_matches(const std::string& title, const std::string& spoken) {
  const std::string a = stem(title), b = stem(spoken);
  return !a.empty() && a == b;
}

bool parse_list_command(const std::string& transcript, ListCommand& out) {
  const std::string t = trim(transcript);
  const std::string lt = lower(t);
  struct Verb { const char* verb; const char* prep; };
  static const Verb kVerbs[] = {{"add ", " to "}, {"put ", " on "}, {"put ", " in "}, {"add ", " on "}};
  for (const auto& v : kVerbs) {
    const std::string verb = v.verb, prep = v.prep;
    if (lt.rfind(verb, 0) != 0) continue;
    const size_t p = lt.rfind(prep);  // last " to " — items may contain "to" ("add toothpaste to …")
    if (p == std::string::npos || p <= verb.size()) continue;
    const std::string what = t.substr(verb.size(), p - verb.size());
    const std::string list = clean_list_name(t.substr(p + prep.size()));
    if (list.empty() || list.size() > 40) continue;
    // Split items on commas and " and ".
    std::vector<std::string> items;
    std::string cur;
    const std::string lw = lower(what);
    for (size_t i = 0; i <= what.size(); ++i) {
      const bool end = i == what.size();
      const bool comma = !end && what[i] == ',';
      const bool and_ = !end && lw.compare(i, 5, " and ") == 0;
      if (end || comma || and_) {
        const std::string item = trim(cur);
        if (!item.empty()) items.push_back(capitalize(item));
        cur.clear();
        if (and_) i += 4;
        continue;
      }
      cur.push_back(what[i]);
    }
    if (items.empty() || items.size() > 12) continue;
    out.list = list;
    out.items = std::move(items);
    return true;
  }
  return false;
}

}  // namespace pocket
