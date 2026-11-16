#include "pocket/app.hpp"
#include "pocket/ui.hpp"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>

namespace pocket {
namespace {


/** Minimal JSON array parse for [{id,title,filename,size},...] — tolerant of spacing. */
void parse_music_list(const std::string& json, std::vector<MusicTrack>& out) {
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

    MusicTrack t;
    grab("id", t.id);
    grab("title", t.title);
    grab("filename", t.filename);
    const size_t sk = obj_s.find("\"size");
    if (sk != std::string::npos) {
      size_t n = obj_s.find(':', sk);
      if (n != std::string::npos) {
        ++n;
        while (n < obj_s.size() && (obj_s[n] == ' ' || obj_s[n] == '\t')) ++n;
        t.size_bytes = std::atoi(obj_s.c_str() + n);
      }
    }
    if (t.id.empty()) {
      i = end + 1;
      continue;
    }
    if (t.title.empty()) t.title = t.filename.empty() ? t.id : t.filename;
    out.push_back(t);
    i = end + 1;
  }
}

}  // namespace

void App::music_load_local() {
  music_tracks_.clear();
  if (!storage_ || !storage_->music_ensure_root()) {
    music_status_ = "No local music yet.";
    return;
  }
  const std::string root = storage_->music_root();
  DIR* dir = ::opendir(root.c_str());
  if (!dir) {
    music_status_ = "No local music yet.";
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
    if (ext != "wav" && ext != "mp3" && ext != "ogg" && ext != "flac") continue;
    const std::string path = root + "/" + fname;
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) continue;
    MusicTrack t;
    t.id = fname;
    t.filename = fname;
    t.title = fname.substr(0, dot);
    t.local_path = path;
    t.size_bytes = static_cast<int>(st.st_size);
    music_tracks_.push_back(std::move(t));
  }
  ::closedir(dir);
  music_status_ = music_tracks_.empty() ? "No local music yet."
                                       : (std::to_string(music_tracks_.size()) + " on device");
}

void App::music_sync_from_cloud() {
  music_status_ = "Syncing…";
  mark_content_dirty();
  if (!wifi_.connected()) {
    music_load_local();
    if (music_tracks_.empty())
      music_status_ = "Sync needs Wi‑Fi. Local tracks still play offline.";
    else
      music_status_ = "Offline · " + std::to_string(music_tracks_.size()) + " local";
    return;
  }
  if (!storage_ || !storage_->music_ensure_root()) {
    music_status_ = "No storage (need SD or free internal).";
    return;
  }
  const std::string root = storage_->music_root();
  const bool on_sd = storage_->music_on_sd();
  constexpr int kInternalMax = 2 * 1024 * 1024;
  const std::string json = cloud_.music_list_json(cfg_.device_id, on_sd);
  if (json.empty()) {
    // Fetch failed — never let a network error hide the tracks already on Pocket.
    music_status_ = "Couldn't reach Pocket Cloud \xC2\xB7 showing tracks on Pocket";
    return;
  }
  std::vector<MusicTrack> remote;
  parse_music_list(json, remote);
  if (remote.empty() && json.find('[') != std::string::npos) {
    music_tracks_.clear();
    music_status_ = "Library empty — upload in Companion.";
    return;
  }
  std::vector<MusicTrack> local;
  int skipped_need_sd = 0;
  for (auto& t : remote) {
    if (!on_sd && t.size_bytes > kInternalMax) {
      ++skipped_need_sd;
      continue;
    }
    const uint64_t free = storage_->free_bytes(root);
    if (t.size_bytes > 0 && free < static_cast<uint64_t>(t.size_bytes) + 64 * 1024) {
      music_status_ = "Not enough free space.";
      break;
    }
    std::string fname = t.filename.empty() ? (t.id + ".wav") : t.filename;
    // Sanitize path segment
    for (char& c : fname) {
      if (c == '/' || c == '\\') c = '_';
    }
    const std::string path = root + "/" + fname;
    struct stat st {};
    // Already synced only if complete: size must match when the cloud reports it.
    if (stat(path.c_str(), &st) == 0 && st.st_size > 0 &&
        (t.size_bytes <= 0 || st.st_size == static_cast<off_t>(t.size_bytes))) {
      t.local_path = path;
      local.push_back(t);
      continue;
    }
    int64_t got = 0;
    if (!cloud_.download_to_file("music", cfg_.device_id, t.id, path, on_sd ? 0 : kInternalMax, &got)) {
      music_status_ = "Download failed for " + t.title;
      continue;
    }
    t.local_path = path;
    t.size_bytes = static_cast<int>(got);
    local.push_back(t);
  }
  music_tracks_ = std::move(local);
  if (music_tracks_.empty()) {
    music_status_ = skipped_need_sd > 0 ? "Larger tracks need an SD card." : "No tracks available.";
  } else if (skipped_need_sd > 0) {
    music_status_ = "Synced " + std::to_string(music_tracks_.size()) + " · some need SD";
  } else {
    music_status_ = "Synced " + std::to_string(music_tracks_.size());
  }
}

void App::render_music() {
  draw_status_bar();
  using ui::Icon;
  using ui::Row;
  using ui::RowKind;
  const ScreenId s = nav_.current();
  if (s == ScreenId::MusicNowPlaying) {
    ui::header(canvas_, "Now playing", true);
    // Sleeve: half-toned square with the note mark on a paper disc.
    constexpr int kArt = 232;
    const int ax = (kCanvasW - kArt) / 2;
    const int ay = kListTop + 4;
    canvas_.fill_round_rect(ax, ay, kArt, kArt, 18, Gray::G0);
    for (int y = ay + 10; y < ay + kArt - 10; y += 6)
      for (int x = ax + 10 + ((y / 6) % 2) * 3; x < ax + kArt - 10; x += 6) canvas_.set_pixel(x, y, Gray::G3);
    canvas_.fill_disc(kCanvasW / 2, ay + kArt / 2, 56, Gray::G3);
    ui::icon(canvas_, Icon::Music, kCanvasW / 2, ay + kArt / 2, 60, Gray::G0);
    int ty = ay + kArt + 24;
    if (music_index_ >= 0 && music_index_ < static_cast<int>(music_tracks_.size())) {
      const auto& t = music_tracks_[static_cast<size_t>(music_index_)];
      const int tw = std::min(kContentW, canvas_.text_width(t.title, Canvas::TextRole::ScreenTitle));
      canvas_.draw_text_fit((kCanvasW - tw) / 2, ty, kContentW, t.title, Canvas::TextRole::ScreenTitle, Gray::G0);
      ty += 54;
    }
    const char* st = music_playing_ ? "Playing" : "Stopped";
    const int sw = canvas_.text_width(st, Canvas::TextRole::Caption);
    canvas_.draw_text((kCanvasW - sw) / 2, ty, st, Canvas::TextRole::Caption, Gray::G0);
    focus_.count = 2;
    ui::row(canvas_, kBottomCtaY - kRowPitch,
            Row{"Stop", RowKind::Action, {}, false, Icon::Stop, true},
            focus_.index == 0);
    ui::row(canvas_, kBottomCtaY, Row{"Back", RowKind::Back}, focus_.index == 1);
    return;
  }

  ui::header(canvas_, "Music", true);
  if (!music_status_.empty()) {
    canvas_.draw_text_fit(kSideMargin, below_title(kContentTop) - 12, kContentW, music_status_,
                          Canvas::TextRole::Caption, Gray::G0);
  }
  const int list_top = kListTopWithMeta;
  const int n = static_cast<int>(music_tracks_.size());
  focus_.count = n + 2;
  if (n == 0) {
    ui::empty_state(canvas_, Icon::Music, "No music yet",
                    wifi_.connected() ? "Add tracks in Companion, then Sync."
                                      : "Local tracks still play offline. Sync needs Wi-Fi.",
                    340);
  }
  // Tracks + Sync + Back scroll as one window so every row is reachable and visible.
  constexpr int kVisible = 9;
  const int total = n + 2;
  const int start = n == 0 ? 0 : ui::window_start(focus_.index, total, kVisible);
  for (int k = 0; k < kVisible && start + k < n; ++k) {
    const int i = start + k;
    ui::row(canvas_, list_top + k * kRowPitch,
            Row{music_tracks_[static_cast<size_t>(i)].title, RowKind::Plain, {}, false, Icon::Music, true},
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

void App::handle_music(InputEvent e) {
  const ScreenId s = nav_.current();
  if (s == ScreenId::MusicNowPlaying) {
    focus_.count = 2;
    if (e == InputEvent::Back || (e == InputEvent::Select && focus_.index == 1)) {
      if (audio_) audio_->stop();
      music_playing_ = false;
      nav_.pop();
      after_nav();
      return;
    }
    if (e == InputEvent::Up || e == InputEvent::Down) {
      const int prev = focus_.index;
      focus_.move(e == InputEvent::Down ? 1 : -1);
      if (focus_.index == prev) return;
      mark_list_focus_dirty(bottom_action_y(1), prev, focus_.index);
    } else if (e == InputEvent::Select && focus_.index == 0) {
      if (audio_) audio_->stop();
      music_playing_ = false;
      mark_content_dirty();
    }
    return;
  }

  const int n = static_cast<int>(music_tracks_.size());
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
      music_index_ = focus_.index;
      const auto& t = music_tracks_[music_index_];
      if (t.local_path.empty()) {
        music_status_ = wifi_.connected() ? "Sync first." : "Needs a local file — Sync over Wi‑Fi.";
        mark_content_dirty();
        return;
      }
      if (audio_ && audio_->play_file(t.local_path)) {
        music_playing_ = true;
        play_sound(SoundId::Click);
        nav_.push(ScreenId::MusicNowPlaying);
        after_nav();
      } else {
        music_status_ = "Couldn't play (need WAV).";
        mark_content_dirty();
      }
    } else if (focus_.index == n) {
      music_sync_from_cloud();
      mark_content_dirty();
    } else {
      nav_.pop();
      after_nav();
    }
  }
}

}  // namespace pocket
