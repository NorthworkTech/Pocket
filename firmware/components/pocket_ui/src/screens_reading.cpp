#include "pocket/app.hpp"
#include "pocket/ui.hpp"
#include "pocket/json_lite.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>

namespace pocket {

namespace {
/** Book page geometry — shared by render and pagination so pages never run under controls. */
constexpr int kReadTop = kContentTop + 40;
constexpr int kReadBottom = kBottomCtaY - 52;
constexpr int kReadGap = 6;
}  // namespace

namespace {


/** Cloud library list: `{"books":[...]}` or a bare array of {id,title,author,format,size}. */
bool parse_books_list(const std::string& json, std::vector<Book>& out) {
  out.clear();
  JsonValue root;
  if (!json_parse(json, root)) {
    // Older firmware's transport handed us the array tail of an object ("[...]}").
    const size_t a = json.find('['), b = json.rfind(']');
    if (a == std::string::npos || b == std::string::npos || b < a || !json_parse(json.substr(a, b - a + 1), root))
      return false;
  }
  const JsonValue* arr = root.is_array() ? &root : root.get("books");
  if (!arr || !arr->is_array()) return false;
  for (const auto& o : arr->items) {
    if (!o.is_object()) continue;
    Book b;
    b.id = o.get_string("id");
    if (b.id.empty()) continue;
    b.title = o.get_string("title");
    b.author = o.get_string("author");
    b.format = o.get_string("format");
    b.filename = o.get_string("filename");
    b.size_bytes = static_cast<int>(o.get_int("size_bytes", o.get_int("size")));
    if (b.title.empty()) b.title = b.filename.empty() ? b.id : b.filename;
    out.push_back(std::move(b));
  }
  return true;
}

/** On-device name for a cloud book: its id (unique, FAT/LittleFS-safe, short). */
std::string cloud_book_file(const std::string& id) {
  std::string f;
  for (char c : id) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') f.push_back(c);
    if (f.size() >= 48) break;
  }
  return (f.empty() ? std::string("book") : f) + ".txt";
}

/** Legacy (≤2.0) on-device name derived from the server filename. */
std::string legacy_book_file(const Book& b) {
  std::string fname = b.filename.empty() ? (b.id + ".txt") : b.filename;
  for (char& c : fname)
    if (c == '/' || c == '\\') c = '_';
  if (fname.size() < 4 || fname.substr(fname.size() - 4) != ".txt") fname += ".txt";
  return fname;
}

constexpr const char* kLibraryIndex = "library.json";

std::string json_escape(const std::string& in) {
  std::string o;
  for (unsigned char c : in) {
    if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(static_cast<char>(c)); }
    else if (c < 0x20) { char u[8]; std::snprintf(u, sizeof(u), "\\u%04x", c); o += u; }
    else o.push_back(static_cast<char>(c));
  }
  return o;
}

/** Index of cloud books on this device: file → title/author/remote size. */
struct IndexEntry {
  std::string id, title, author, format, file;
  int remote_size = 0;
};

std::vector<IndexEntry> read_index(const std::string& root) {
  std::vector<IndexEntry> out;
  std::string body;
  if (!read_file_recover(root + "/" + kLibraryIndex, body)) return out;
  JsonValue v;
  if (!json_parse(body, v) || !v.is_array()) return out;
  for (const auto& o : v.items) {
    if (!o.is_object()) continue;
    IndexEntry e;
    e.id = o.get_string("id");
    e.title = o.get_string("title");
    e.author = o.get_string("author");
    e.format = o.get_string("format");
    e.file = o.get_string("file");
    e.remote_size = static_cast<int>(o.get_int("remote_size"));
    if (!e.file.empty()) out.push_back(std::move(e));
  }
  return out;
}

void write_index(const std::string& root, const std::vector<IndexEntry>& idx) {
  std::string body = "[";
  for (size_t i = 0; i < idx.size(); ++i) {
    const auto& e = idx[i];
    char num[24];
    std::snprintf(num, sizeof(num), "%d", e.remote_size);
    body += std::string(i ? "," : "") + "{\"id\":\"" + json_escape(e.id) + "\",\"title\":\"" + json_escape(e.title) +
            "\",\"author\":\"" + json_escape(e.author) + "\",\"format\":\"" + json_escape(e.format) +
            "\",\"file\":\"" + json_escape(e.file) + "\",\"remote_size\":" + num + "}";
  }
  body += "]";
  (void)write_file_atomic(root + "/" + kLibraryIndex, body);
}

/** Clean a downloaded book for the panel: no BOM/CR, and unwrap hard-wrapped plain text
 * (Gutenberg-style ~70-column lines) so paragraphs flow to the screen width. */
void normalize_book_text(std::string& t) {
  if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xEF && static_cast<unsigned char>(t[1]) == 0xBB &&
      static_cast<unsigned char>(t[2]) == 0xBF)
    t.erase(0, 3);
  size_t w = 0;
  for (size_t r = 0; r < t.size(); ++r) {
    if (t[r] == '\r') {
      if (r + 1 < t.size() && t[r + 1] == '\n') continue;
      t[w++] = '\n';
    } else {
      t[w++] = t[r];
    }
  }
  t.resize(w);
  // Detect hard wrap: most non-empty lines between 45 and 80 bytes.
  size_t lines = 0, wrapped = 0, start = 0;
  for (size_t i = 0; i <= t.size() && lines < 4000; ++i) {
    if (i == t.size() || t[i] == '\n') {
      const size_t len = i - start;
      if (len > 0) {
        ++lines;
        if (len >= 45 && len <= 80) ++wrapped;
      }
      start = i + 1;
    }
  }
  if (lines < 20 || wrapped * 10 < lines * 6) return;
  // Single newline inside a paragraph → space; blank lines (paragraph breaks) stay.
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] != '\n') continue;
    const bool prev_nl = i > 0 && t[i - 1] == '\n';
    const bool next_nl = i + 1 < t.size() && t[i + 1] == '\n';
    const bool next_indent = i + 1 < t.size() && (t[i + 1] == ' ' || t[i + 1] == '\t');
    if (!prev_nl && !next_nl && !next_indent) t[i] = ' ';
  }
}

}  // namespace

void App::reading_load_local() {
  books_.clear();
  if (!storage_ || !storage_->book_ensure_root()) {
    reading_status_ = "No local books yet.";
    return;
  }
  const std::string root = storage_->book_root();
  // Cloud books first, with their real titles and authors.
  std::vector<std::string> known;
  for (const auto& e : read_index(root)) {
    const std::string path = root + "/" + e.file;
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) continue;
    Book b;
    b.id = e.id.empty() ? e.file : e.id;
    b.title = e.title.empty() ? e.file : e.title;
    b.author = e.author;
    b.format = e.format;
    b.filename = e.file;
    b.local_path = path;
    b.size_bytes = static_cast<int>(st.st_size);
    known.push_back(e.file);
    books_.push_back(std::move(b));
  }
  // Then anything copied onto the card by hand.
  if (DIR* dir = ::opendir(root.c_str())) {
    while (auto* ent = ::readdir(dir)) {
      const char* name = ent->d_name;
      if (!name || name[0] == '.') continue;
      const std::string fname = name;
      if (std::find(known.begin(), known.end(), fname) != known.end()) continue;
      const auto dot = fname.rfind('.');
      if (dot == std::string::npos) continue;
      std::string ext = fname.substr(dot + 1);
      for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (ext != "txt") continue;
      const std::string path = root + "/" + fname;
      struct stat st {};
      if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) continue;
      Book b;
      b.id = fname;
      b.filename = fname;
      b.title = fname.substr(0, dot);
      b.format = "txt";
      b.local_path = path;
      b.size_bytes = static_cast<int>(st.st_size);
      books_.push_back(std::move(b));
    }
    ::closedir(dir);
  }
  reading_status_ =
      books_.empty() ? "No local books yet." : (std::to_string(books_.size()) + " on device");
}

void App::reading_sync_from_cloud() {
  reading_status_ = "Syncing\xE2\x80\xA6";
  mark_content_dirty();
  if (!wifi_.connected()) {
    reading_load_local();
    if (books_.empty())
      reading_status_ = "Sync needs Wi-Fi. Local books still open offline.";
    else
      reading_status_ = "Offline \xC2\xB7 " + std::to_string(books_.size()) + " local";
    return;
  }
  if (!storage_ || !storage_->book_ensure_root()) {
    reading_status_ = "No storage (need SD or free internal).";
    return;
  }
  const std::string root = storage_->book_root();
  show_status("Syncing your library\xE2\x80\xA6");
  const std::string json = cloud_.books_list_json(cfg_.device_id);
  if (json.empty()) {
    reading_load_local();
    reading_status_ = "Couldn't reach Pocket Cloud \xC2\xB7 showing books on Pocket";
    return;
  }
  std::vector<Book> remote;
  if (!parse_books_list(json, remote)) {
    // Never treat a garbled reply as "library is empty" — that would delete books.
    reading_load_local();
    reading_status_ = "Pocket Cloud sent an unexpected reply Â· showing books on Pocket";
    return;
  }
  const std::vector<IndexEntry> old_idx = read_index(root);
  std::vector<IndexEntry> idx;
  int failed = 0;
  std::string fail_msg;
  for (const auto& b : remote) {
    const std::string file = cloud_book_file(b.id);
    const std::string path = root + "/" + file;
    // Same book, same size on the server → keep the local copy; edited → fetch again.
    int prev_remote = -1;
    for (const auto& e : old_idx)
      if (e.id == b.id) prev_remote = e.remote_size;
    struct stat st {};
    const bool have = stat(path.c_str(), &st) == 0 && st.st_size > 0;
    if (!have || prev_remote != b.size_bytes) {
      const uint64_t free = storage_->free_bytes(root);
      if (b.size_bytes > 0 && free < static_cast<uint64_t>(b.size_bytes) + 64 * 1024) {
        ++failed;
        fail_msg = "Not enough free space for \xE2\x80\x9C" + b.title + "\xE2\x80\x9D.";
        continue;
      }
      show_status("Downloading \xE2\x80\x9C" + b.title + "\xE2\x80\x9D\xE2\x80\xA6");
      int64_t got = 0;
      if (!cloud_.download_to_file("book", cfg_.device_id, b.id, path, 8LL * 1024 * 1024, &got)) {
        ++failed;
        fail_msg = "Couldn't download \xE2\x80\x9C" + b.title + "\xE2\x80\x9D.";
        if (!have) continue;  // keep an older copy if we had one
      }
    }
    // Pre-2.1 builds saved by server filename — drop that copy so it isn't listed twice.
    const std::string legacy = root + "/" + legacy_book_file(b);
    if (legacy != path) ::remove(legacy.c_str());
    idx.push_back({b.id, b.title, b.author, b.format, file, b.size_bytes});
  }
  // Books removed in Pocket Cloud leave the device too (only ones the cloud put there).
  for (const auto& e : old_idx) {
    bool still = false;
    for (const auto& n : idx) still = still || n.file == e.file;
    if (!still) ::remove((root + "/" + e.file).c_str());
  }
  write_index(root, idx);
  reading_load_local();
  if (failed) {
    reading_status_ = failed == 1 ? fail_msg : std::to_string(failed) + " books couldn't download.";
  } else {
    reading_status_ = books_.empty() ? "Library empty \xE2\x80\x94 add books in Companion."
                                     : "Up to date \xC2\xB7 " + std::to_string(books_.size()) +
                                           (books_.size() == 1 ? " book" : " books");
  }
}

void App::reading_paginate() {
  reading_pages_.clear();
  reading_pages_.push_back(0);
  const std::string& text = reading_text_;
  const size_t n = text.size();
  if (n == 0) return;

  const int max_w = kContentW;
  const int line_h = canvas_.text_height(Canvas::TextRole::Reading) + kReadGap;
  const int top = kReadTop;
  const int bottom = kReadBottom;
  const int lines_per_page = std::max(1, (bottom - top) / std::max(1, line_h));

  size_t i = 0;
  int line_count = 0;
  auto advance_line = [&]() {
    ++line_count;
    if (line_count >= lines_per_page) {
      line_count = 0;
      if (i < n) reading_pages_.push_back(i);
    }
  };

  while (i < n) {
    while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i >= n) break;
    if (text[i] == '\n') {
      ++i;
      advance_line();
      continue;
    }
    const size_t line_start = i;
    size_t last_break = i;
    size_t j = i;
    while (j < n && text[j] != '\n') {
      size_t word_end = j;
      while (word_end < n && text[word_end] != ' ' && text[word_end] != '\t' && text[word_end] != '\n') {
        ++word_end;
      }
      const std::string_view candidate(text.data() + line_start, word_end - line_start);
      if (canvas_.text_width(candidate, Canvas::TextRole::Reading) <= max_w) {
        last_break = word_end;
        j = word_end;
        while (j < n && (text[j] == ' ' || text[j] == '\t')) ++j;
      } else if (last_break == line_start) {
        size_t cut = line_start + 1;
        while (cut < word_end &&
              canvas_.text_width(std::string_view(text.data() + line_start, cut - line_start),
                                  Canvas::TextRole::Reading) <= max_w) {
          ++cut;
        }
        if (cut > line_start + 1) --cut;
        last_break = cut;
        j = cut;
        break;
      } else {
        break;
      }
    }
    if (last_break > line_start) {
      i = last_break;
      while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
      if (i < n && text[i] == '\n') ++i;  // the newline ends this line, not a blank one
      advance_line();
    } else if (j < n && text[j] == '\n') {
      i = j + 1;
      advance_line();
    } else {
      break;
    }
  }
}

void App::reading_open_book(int index) {
  if (index < 0 || index >= static_cast<int>(books_.size())) return;
  const auto& b = books_[index];
  if (b.local_path.empty()) {
    reading_status_ = wifi_.connected() ? "Sync first." : "Needs a local file — Sync over Wi‑Fi.";
    mark_content_dirty();
    return;
  }
  struct stat st {};
  if (stat(b.local_path.c_str(), &st) != 0 || st.st_size <= 0) {
    reading_status_ = "Couldn't open that book.";
    mark_content_dirty();
    return;
  }
  FILE* f = std::fopen(b.local_path.c_str(), "rb");
  if (!f) {
    reading_status_ = "Couldn't open that book.";
    mark_content_dirty();
    return;
  }
  if (st.st_size > 256 * 1024) show_status("Opening \xE2\x80\x9C" + b.title + "\xE2\x80\x9D\xE2\x80\xA6");
  constexpr size_t kMaxBookBytes = 3 * 1024 * 1024;  // generous for a normalized-text novel
  // Free the previous book before allocating (no doubling growth, no peak of two books).
  reading_text_.clear();
  reading_text_.shrink_to_fit();
  reading_pages_.clear();
  reading_pages_.shrink_to_fit();
  const size_t want = std::min(static_cast<size_t>(st.st_size), kMaxBookBytes);
  reading_text_.resize(want);
  const size_t got = std::fread(reading_text_.data(), 1, want, f);
  std::fclose(f);
  reading_text_.resize(got);
  normalize_book_text(reading_text_);
  // Page breaks are expensive to compute on device for a novel — reuse them when the book
  // and the page layout haven't changed.
  const std::string cache_path = b.local_path + ".pages";
  char key[96];
  std::snprintf(key, sizeof(key), "PG2 %u %d %d %d\n", static_cast<unsigned>(reading_text_.size()), kContentW,
                canvas_.text_height(Canvas::TextRole::Reading), kReadBottom - kReadTop);
  bool cached = false;
  {
    std::string blob;
    if (read_file_recover(cache_path, blob) && blob.rfind(key, 0) == 0) {
      const size_t hdr = std::strlen(key);
      const size_t n = (blob.size() - hdr) / 4;
      std::vector<size_t> pages;
      pages.reserve(n);
      size_t prev = 0;
      bool ok = n > 0;
      for (size_t i = 0; ok && i < n; ++i) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(blob.data() + hdr + i * 4);
        const size_t off = static_cast<size_t>(p[0]) | (static_cast<size_t>(p[1]) << 8) |
                           (static_cast<size_t>(p[2]) << 16) | (static_cast<size_t>(p[3]) << 24);
        if ((i == 0 && off != 0) || (i > 0 && off <= prev) || off >= reading_text_.size() + 1) ok = false;
        prev = off;
        pages.push_back(off);
      }
      if (ok) {
        reading_pages_ = std::move(pages);
        cached = true;
      }
    }
  }
  if (!cached) {
    reading_paginate();
    std::string blob = key;
    for (size_t off : reading_pages_) {
      const uint32_t v = static_cast<uint32_t>(off);
      blob.push_back(static_cast<char>(v & 0xFF));
      blob.push_back(static_cast<char>((v >> 8) & 0xFF));
      blob.push_back(static_cast<char>((v >> 16) & 0xFF));
      blob.push_back(static_cast<char>((v >> 24) & 0xFF));
    }
    (void)write_file_atomic(cache_path, blob);
  }
  reading_page_ = 0;
  // Resume key is the on-device filename: stable whether the library came from local
  // storage (id = filename) or a cloud sync (id = cloud uuid).
  const std::string resume_key = b.filename.empty() ? b.id : b.filename;
  if ((cfg_.reading_last_book_id == resume_key || cfg_.reading_last_book_id == b.id) &&
      cfg_.reading_last_page < static_cast<int>(reading_pages_.size())) {
    reading_page_ = cfg_.reading_last_page;
  }
  book_index_ = index;
  reading_page_shown_ms_ = now_ms_;
  play_sound(SoundId::Click);
  nav_.push(ScreenId::ReadingBook);
  after_nav();
  focus_.index = 1;  // "Next" — the first press turns the page
}

void App::render_reading() {
  draw_status_bar();
  using ui::Icon;
  using ui::Row;
  using ui::RowKind;
  const ScreenId s = nav_.current();

  if (s == ScreenId::ReadingBook) {
    if (book_index_ < 0 || book_index_ >= static_cast<int>(books_.size()) || reading_pages_.empty()) {
      ui::header(canvas_, "Reading", true);
      ui::empty_state(canvas_, Icon::Reading, "Can't open this book", "Try syncing it again from Companion.", 360);
      focus_.count = 3;
      ui::button(canvas_, kBottomCtaY, "Back", true, Icon::ChevronLeft, true);
      return;
    }
    const auto& b = books_[static_cast<size_t>(book_index_)];
    // Running head.
    canvas_.draw_text_fit(kSideMargin, kContentTop - 6, kContentW, b.title, Canvas::TextRole::Caption, Gray::G0);
    canvas_.dotted_hline(kSideMargin, kContentTop + 24, kContentW, Gray::G0, 3);
    const size_t start = reading_pages_[static_cast<size_t>(reading_page_)];
    const size_t stop = (static_cast<size_t>(reading_page_) + 1 < reading_pages_.size())
                            ? reading_pages_[static_cast<size_t>(reading_page_) + 1]
                            : reading_text_.size();
    canvas_.draw_text_wrapped(kSideMargin, kReadTop, kContentW, kReadGap,
                              reading_text_.substr(start, stop - start), Canvas::TextRole::Reading, Gray::G0);
    // Page indicator + progress hairline.
    const int total = static_cast<int>(reading_pages_.size());
    char pg[96];
    const std::string left = reading_time_left_label();
    std::snprintf(pg, sizeof(pg), "%d of %d%s%s", reading_page_ + 1, total, left.empty() ? "" : " \xC2\xB7 ",
                  left.c_str());
    const int py = kBottomCtaY - 40;
    const int pw = canvas_.text_width(pg, Canvas::TextRole::Caption);
    canvas_.draw_text(kCanvasW - kSideMargin - pw, py, pg, Canvas::TextRole::Caption, Gray::G0);
    const int bar_w = kContentW - pw - 16;
    canvas_.hline(kSideMargin, py + 13, bar_w, Gray::G0);
    const int done = bar_w * (reading_page_ + 1) / std::max(1, total);
    canvas_.fill_rect(kSideMargin, py + 11, std::max(4, done), 5, Gray::G0);
    // Compact control bar: Prev · Next · Close.
    focus_.count = 3;
    static const char* kActs[3] = {"Prev", "Next", "Close"};
    static const Icon kIcons[3] = {Icon::ChevronLeft, Icon::ChevronRight, Icon::ChevronLeft};
    const int seg = (kFocusRowW - 16) / 3;
    for (int i = 0; i < 3; ++i) {
      const int x = kSideMargin + i * (seg + 8);
      const bool f = focus_.index == i;
      if (f) canvas_.fill_round_rect(x, kBottomCtaY, seg, kFocusRowH, kFocusRowH / 2, Gray::G0);
      else canvas_.stroke_round_rect(x, kBottomCtaY, seg, kFocusRowH, kFocusRowH / 2, Gray::G0, 2, Gray::G3);
      const Gray ink = f ? Gray::G3 : Gray::G0;
      const int lw = canvas_.text_width(kActs[i], Canvas::TextRole::BodyStrong);
      const bool icon_after = i == 1;
      const int total_w = lw + (i == 2 ? 0 : 26);
      int lx = x + (seg - total_w) / 2;
      if (i == 0) {
        ui::icon(canvas_, kIcons[i], lx + 8, kBottomCtaY + kFocusRowH / 2, 18, ink);
        lx += 26;
      }
      canvas_.draw_text(lx, kBottomCtaY + (kFocusRowH - canvas_.text_height(Canvas::TextRole::BodyStrong)) / 2,
                        kActs[i], Canvas::TextRole::BodyStrong, ink);
      if (icon_after) ui::icon(canvas_, kIcons[i], lx + lw + 16, kBottomCtaY + kFocusRowH / 2, 18, ink);
    }
    return;
  }

  ui::header(canvas_, "Reading", true);
  if (!reading_status_.empty()) {
    const std::string stats = reading_summary_line();
    canvas_.draw_text_fit(kSideMargin, below_title(kContentTop) - 12, kContentW,
                          stats.empty() ? reading_status_ : stats, Canvas::TextRole::Caption, Gray::G0);
  }
  const int list_top = kListTopWithMeta;
  const int n = static_cast<int>(books_.size());
  focus_.count = n + 2;
  if (n == 0) {
    ui::empty_state(canvas_, Icon::Reading, "No books yet",
                    wifi_.connected() ? "Add EPUB or text files in Companion, then Sync."
                                      : "Local books still open offline. Sync needs Wi-Fi.",
                    340);
  }
  constexpr int kVisible = 9;
  const int total = n + 2;
  const int start = n == 0 ? 0 : ui::window_start(focus_.index, total, kVisible);
  for (int k = 0; k < kVisible && start + k < n; ++k) {
    const int i = start + k;
    const auto& bk = books_[static_cast<size_t>(i)];
    ui::row(canvas_, list_top + k * kRowPitch,
            Row{bk.title, RowKind::Value, bk.author.empty() ? bk.format : bk.author, false, Icon::Reading, true},
            i == focus_.index);
  }
  if (n > 0) ui::scrollbar(canvas_, list_top, kVisible * kRowPitch - 8, start, kVisible, total);
  const int sync_slot = n - start;
  const int y = n == 0 ? kBottomCtaY - kRowPitch : list_top + sync_slot * kRowPitch;
  if (n > 0 && sync_slot + 1 >= kVisible + 1) return;  // actions scrolled out of view
  ui::row(canvas_, y,
          Row{wifi_.connected() ? "Sync from Companion" : "Sync", RowKind::Value, wifi_.connected() ? "" : "Needs Wi-Fi",
              false, Icon::Update, true},
          focus_.index == n);
  ui::row(canvas_, y + kRowPitch, Row{"Back", RowKind::Back}, focus_.index == n + 1);
}

void App::handle_reading(InputEvent e) {
  const ScreenId s = nav_.current();

  if (s == ScreenId::ReadingBook) {
    focus_.count = 3;
    // Persist progress on any exit from the book so a reboot resumes here.
    auto save_progress = [&]() {
      if (book_index_ >= 0 && book_index_ < static_cast<int>(books_.size())) {
        cfg_.reading_last_book_id =
            books_[book_index_].filename.empty() ? books_[book_index_].id : books_[book_index_].filename;
        cfg_.reading_last_page = reading_page_;
        store_.save(cfg_);
        reading_progress_dirty_ = true;
      }
    };
    if (e == InputEvent::Back) {
      reading_note_page_left(false);
      reading_stats_save();
      save_progress();
      maybe_push_reading_progress(true);
      nav_.pop();
      after_nav();
      return;
    }
    if (e == InputEvent::Up || e == InputEvent::Down) {
      const int prev = focus_.index;
      focus_.move(e == InputEvent::Down ? 1 : -1);
      if (focus_.index == prev) return;
      // Single-row control bar.
      mark_region_dirty(0, kBottomCtaY - 6, kCanvasW, kFocusRowH + 12);
    } else if (e == InputEvent::Select) {
      if (focus_.index == 0) {
        if (reading_page_ > 0) {
          reading_note_page_left(false);
          --reading_page_;
          save_progress();
          mark_content_dirty();
        }
      } else if (focus_.index == 1) {
        if (reading_page_ + 1 < static_cast<int>(reading_pages_.size())) {
          reading_note_page_left(true);
          ++reading_page_;
          save_progress();
          mark_content_dirty();
        }
      } else {
        reading_note_page_left(false);
        reading_stats_save();
        save_progress();
        maybe_push_reading_progress(true);
        nav_.pop();
        after_nav();
      }
    }
    return;
  }

  const int n = static_cast<int>(books_.size());
  focus_.count = n + 2;
  if (e == InputEvent::Back) {
    nav_.pop();
    after_nav();
    return;
  }
  if (e == InputEvent::Up || e == InputEvent::Down) {
    const int prev = focus_.index;
    focus_.move(e == InputEvent::Down ? 1 : -1);
    if (focus_.index == prev) return;
    // Empty library parks Sync/Back at the bottom CTA band — not list_top.
    mark_content_dirty();  // windowed list: rows move when it scrolls
  } else if (e == InputEvent::Select) {
    if (focus_.index < n) {
      reading_open_book(focus_.index);
    } else if (focus_.index == n) {
      reading_sync_from_cloud();
      mark_content_dirty();
    } else {
      nav_.pop();
      after_nav();
    }
  }
}

}  // namespace pocket

namespace pocket {

void App::maybe_push_reading_progress(bool force) {
  if (!reading_progress_dirty_) return;
  if (!cfg_.companion_linked || !wifi_.connected() || cfg_.device_id.empty()) return;
  // While reading, report at most every 2 minutes; closing the book reports right away.
  if (!force && last_reading_push_ms_ != 0 && now_ms_ - last_reading_push_ms_ < 120000u) return;
  if (book_index_ < 0 || book_index_ >= static_cast<int>(books_.size()) || reading_pages_.empty()) return;
  last_reading_push_ms_ = now_ms_;
  const int pages = static_cast<int>(reading_pages_.size());
  const int page = std::clamp(reading_page_, 0, pages - 1);
  const int percent = pages <= 1 ? 100 : (page * 100) / (pages - 1);
  if (cloud_.post_reading_progress(cfg_.device_id, books_[static_cast<size_t>(book_index_)].id, page + 1, pages,
                                   percent)) {
    reading_progress_dirty_ = false;
  }
}

}  // namespace pocket
