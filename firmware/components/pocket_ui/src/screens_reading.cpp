#include "pocket/app.hpp"
#include "pocket/ui.hpp"
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


/** Minimal JSON array parse for [{id,title,author,format,filename,size},...] — tolerant of spacing. */
void parse_books_list(const std::string& json, std::vector<Book>& out) {
  out.clear();
  size_t i = 0;
  while (i < json.size()) {
    const size_t obj = json.find('{', i);
    if (obj == std::string::npos) break;
    const size_t end = json.find('}', obj + 1);
    if (end == std::string::npos) break;
    const std::string obj_s = json.substr(obj, end - obj + 1);

    auto grab = [&](const char* key, std::string& dest) {
      const std::string needle = std::string("\"") + key + "\"";
      const size_t k = obj_s.find(needle);
      if (k == std::string::npos) return;
      const size_t colon = obj_s.find(':', k + needle.size());
      if (colon == std::string::npos) return;
      const size_t q1 = obj_s.find('"', colon + 1);
      if (q1 == std::string::npos) return;
      const size_t q2 = obj_s.find('"', q1 + 1);
      if (q2 == std::string::npos) return;
      dest = obj_s.substr(q1 + 1, q2 - q1 - 1);
    };

    Book b;
    grab("id", b.id);
    grab("title", b.title);
    grab("author", b.author);
    grab("format", b.format);
    grab("filename", b.filename);
    const size_t sk = obj_s.find("\"size");
    if (sk != std::string::npos) {
      size_t n = obj_s.find(':', sk);
      if (n != std::string::npos) {
        ++n;
        while (n < obj_s.size() && (obj_s[n] == ' ' || obj_s[n] == '\t')) ++n;
        b.size_bytes = std::atoi(obj_s.c_str() + n);
      }
    }
    if (b.id.empty()) {
      i = end + 1;
      continue;
    }
    if (b.title.empty()) b.title = b.filename.empty() ? b.id : b.filename;
    out.push_back(b);
    i = end + 1;
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
  DIR* dir = ::opendir(root.c_str());
  if (!dir) {
    reading_status_ = "No local books yet.";
    return;
  }
  while (auto* ent = ::readdir(dir)) {
    const char* name = ent->d_name;
    if (!name || name[0] == '.') continue;
    const std::string fname = name;
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
  reading_status_ =
      books_.empty() ? "No local books yet." : (std::to_string(books_.size()) + " on device");
}

void App::reading_sync_from_cloud() {
  reading_status_ = "Syncing…";
  mark_content_dirty();
  if (!wifi_.connected()) {
    reading_load_local();
    if (books_.empty())
      reading_status_ = "Sync needs Wi‑Fi. Local books still open offline.";
    else
      reading_status_ = "Offline · " + std::to_string(books_.size()) + " local";
    return;
  }
  if (!storage_ || !storage_->book_ensure_root()) {
    reading_status_ = "No storage (need SD or free internal).";
    return;
  }
  const std::string root = storage_->book_root();
  const std::string json = cloud_.books_list_json(cfg_.device_id);
  if (json.empty()) {
    reading_status_ = "Couldn't reach Pocket Cloud \xC2\xB7 showing books on Pocket";
    return;
  }
  std::vector<Book> remote;
  parse_books_list(json, remote);
  if (remote.empty() && json.find('[') != std::string::npos) {
    books_.clear();
    reading_status_ = "Library empty — upload in Companion.";
    return;
  }
  std::vector<Book> local;
  for (auto& b : remote) {
    const uint64_t free = storage_->free_bytes(root);
    if (b.size_bytes > 0 && free < static_cast<uint64_t>(b.size_bytes) + 64 * 1024) {
      reading_status_ = "Not enough free space.";
      break;
    }
    std::string fname = b.filename.empty() ? (b.id + ".txt") : b.filename;
    for (char& c : fname) {
      if (c == '/' || c == '\\') c = '_';
    }
    if (!fname.empty() && fname.substr(fname.size() >= 4 ? fname.size() - 4 : 0) != ".txt") {
      fname += ".txt";  // device always stores normalized plain text
    }
    const std::string path = root + "/" + fname;
    struct stat st {};
    if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
      b.local_path = path;
      local.push_back(b);
      continue;
    }
    int64_t got = 0;
    if (!cloud_.download_to_file("book", cfg_.device_id, b.id, path, 8LL * 1024 * 1024, &got)) {
      reading_status_ = "Download failed for " + b.title;
      continue;
    }
    b.local_path = path;
    b.size_bytes = static_cast<int>(got);
    local.push_back(b);
  }
  books_ = std::move(local);
  reading_status_ = books_.empty() ? "No books available." : "Synced " + std::to_string(books_.size());
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
  std::ifstream in(b.local_path, std::ios::binary);
  if (!in) {
    reading_status_ = "Couldn't open that book.";
    mark_content_dirty();
    return;
  }
  constexpr size_t kMaxBookBytes = 3 * 1024 * 1024;  // generous for a normalized-text novel
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (text.size() > kMaxBookBytes) text.resize(kMaxBookBytes);
  reading_text_ = std::move(text);
  reading_paginate();
  reading_page_ = 0;
  // Resume key is the on-device filename: stable whether the library came from local
  // storage (id = filename) or a cloud sync (id = cloud uuid).
  const std::string resume_key = b.filename.empty() ? b.id : b.filename;
  if ((cfg_.reading_last_book_id == resume_key || cfg_.reading_last_book_id == b.id) &&
      cfg_.reading_last_page < static_cast<int>(reading_pages_.size())) {
    reading_page_ = cfg_.reading_last_page;
  }
  book_index_ = index;
  play_sound(SoundId::Click);
  nav_.push(ScreenId::ReadingBook);
  after_nav();
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
    char pg[32];
    std::snprintf(pg, sizeof(pg), "%d of %d", reading_page_ + 1, total);
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
    canvas_.draw_text_fit(kSideMargin, below_title(kContentTop) - 12, kContentW, reading_status_,
                          Canvas::TextRole::Caption, Gray::G0);
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
      }
    };
    if (e == InputEvent::Back) {
      save_progress();
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
          --reading_page_;
          save_progress();
          mark_content_dirty();
        }
      } else if (focus_.index == 1) {
        if (reading_page_ + 1 < static_cast<int>(reading_pages_.size())) {
          ++reading_page_;
          save_progress();
          mark_content_dirty();
        }
      } else {
        save_progress();
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
