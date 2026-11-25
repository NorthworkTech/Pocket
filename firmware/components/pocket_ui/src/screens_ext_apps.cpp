/** Apps from Northwork (pocket-contract v1.0 §C–E): read-only feed screens on Home tiles. */
#include "pocket/app.hpp"
#include "pocket/json_lite.hpp"
#include "pocket/ui.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace pocket {

namespace {
constexpr int kExtVisible = 9;

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

/** The blocks focus can rest on (section headers are skipped). */
bool focusable(const ExtBlock& b) { return b.kind != ExtBlock::Kind::Header; }
}  // namespace

void App::ext_apps_load() {
  if (!storage_) return;
  const std::string root = storage_->data_root();
  if (root.empty()) return;
  std::string st;
  if (read_file_recover(root + "/apps.json", st)) (void)ext_apps_parse(st, ext_apps_);
}

bool App::ext_apps_refresh(bool visible) {
  ext_sync_pending_ = false;
  ext_last_fetch_ms_ = now_ms_ == 0 ? 1 : now_ms_;
  if (!wifi_.connected() || cfg_.device_id.empty() || cfg_.device_token.empty()) return false;
  if (visible) show_status("Updating apps\xE2\x80\xA6");
  const std::string body = cloud_.apps_json(cfg_.device_id);
  std::vector<ExtApp> fresh;
  if (body.empty() || !ext_apps_parse(body, fresh)) return false;
  ext_apps_ = std::move(fresh);
  if (!ext_apps_rev_seen_.empty()) ext_apps_rev_ = ext_apps_rev_seen_;
  if (storage_ && storage_->data_ensure_root()) (void)write_file_atomic(storage_->data_root() + "/apps.json", body);
  // The open app may have been removed.
  if ((nav_.current() == ScreenId::ExtApp || nav_.current() == ScreenId::ExtAppText) &&
      ext_app_index_ >= static_cast<int>(ext_apps_.size())) {
    nav_.reset(ScreenId::Home);
  }
  return true;
}

const ExtApp* App::today_ext_app() const {
  for (const auto& a : ext_apps_)
    if (a.today && a.status == "ok" && !a.today_line.empty()) return &a;
  return nullptr;
}

void App::open_ext_app(int index) {
  if (index < 0 || index >= static_cast<int>(ext_apps_.size())) return;
  const std::string id = ext_apps_[static_cast<size_t>(index)].id;
  // Opening pulls a fresh copy when the last one is over 5 minutes old (visibly).
  if (wifi_.connected() && (ext_last_fetch_ms_ == 0 || now_ms_ - ext_last_fetch_ms_ > 5u * 60u * 1000u)) {
    (void)ext_apps_refresh(true);
  }
  ext_app_index_ = -1;
  for (size_t i = 0; i < ext_apps_.size(); ++i)
    if (ext_apps_[i].id == id) ext_app_index_ = static_cast<int>(i);
  if (ext_app_index_ < 0) {
    nav_.reset(ScreenId::Home);
    after_nav();
    return;
  }
  ext_screen_stack_.clear();
  ext_focus_stack_.clear();
  const ExtApp& app = ext_apps_[static_cast<size_t>(ext_app_index_)];
  if (!app.screens.empty()) ext_screen_stack_.push_back(app.screens.front().id);
  focus_.index = 0;
  nav_.push(ScreenId::ExtApp);
  after_nav();
}

void App::render_ext_app() {
  draw_status_bar();
  if (ext_app_index_ < 0 || ext_app_index_ >= static_cast<int>(ext_apps_.size())) {
    ui::header(canvas_, "Apps", true);
    ui::empty_state(canvas_, ui::Icon::Info, "This app was removed", "It's no longer on this Pocket.");
    return;
  }
  const ExtApp& app = ext_apps_[static_cast<size_t>(ext_app_index_)];

  if (nav_.current() == ScreenId::ExtAppText) {
    ui::header(canvas_, ext_text_title_, true);
    canvas_.draw_text_wrapped(kSideMargin, kListTop, kContentW, kWrapGap, ext_text_, Canvas::TextRole::Body, Gray::G0);
    return;
  }

  if (app.status != "ok" || app.screens.empty()) {
    ui::header(canvas_, app.name, true);
    if (app.status == "connect" || app.status == "reconnect") {
      ui::empty_state(canvas_, ui::Icon::Phone, app.status == "reconnect" ? "Sign in again" : "Sign in on your phone",
                      "Open Pocket Companion, then Account \xE2\x86\x92 Apps, and sign in to " + app.name + ".");
    } else {
      ui::empty_state(canvas_, ui::Icon::Cloud, "Getting " + app.name + " ready",
                      "It appears here after your Pocket's next check-in.");
    }
    return;
  }

  const ExtScreen* sc = ext_find_screen(app, ext_screen_stack_.empty() ? std::string() : ext_screen_stack_.back());
  if (!sc) return;
  ui::header(canvas_, sc->title.empty() ? app.name : sc->title, true);
  const int n = static_cast<int>(sc->blocks.size());
  focus_.count = std::max(1, n);
  if (n == 1 && sc->blocks[0].kind == ExtBlock::Kind::Empty) {
    ui::empty_state(canvas_, ui::Icon::Check, sc->blocks[0].text, "");
    return;
  }
  if (focus_.index >= n) focus_.index = 0;
  while (n > 0 && !focusable(sc->blocks[static_cast<size_t>(focus_.index)]) && focus_.index + 1 < n) ++focus_.index;
  const int start = n == 0 ? 0 : ui::window_start(focus_.index, n, kExtVisible);
  for (int k = 0; k < kExtVisible && start + k < n; ++k) {
    const int i = start + k;
    const ExtBlock& b = sc->blocks[static_cast<size_t>(i)];
    const int y = kListTop + k * kRowPitch;
    const bool f = i == focus_.index;
    using ui::Row;
    using ui::RowKind;
    switch (b.kind) {
      case ExtBlock::Kind::Header:
        ui::section(canvas_, y + 20, upper(b.text));
        break;
      case ExtBlock::Kind::Kv:
        ui::row(canvas_, y, Row{b.text, RowKind::Value, b.value}, f);
        break;
      case ExtBlock::Kind::Row:
        ui::row(canvas_, y, Row{b.text, b.go.empty() ? (b.value.empty() ? RowKind::Plain : RowKind::Value) : RowKind::Nav,
                                b.value},
                f);
        break;
      case ExtBlock::Kind::Text:
        ui::row(canvas_, y, Row{b.text, RowKind::Nav}, f);
        break;
      case ExtBlock::Kind::Empty:
        ui::row(canvas_, y, Row{b.text, RowKind::Plain}, f);
        break;
    }
  }
  ui::scrollbar(canvas_, kListTop, kExtVisible * kRowPitch - 8, start, kExtVisible, n);
}

void App::handle_ext_app(InputEvent e) {
  if (e == InputEvent::Back) {
    if (nav_.current() == ScreenId::ExtAppText) {
      nav_.pop();
      const int back_to = ext_focus_stack_.empty() ? 0 : ext_focus_stack_.back();
      if (!ext_focus_stack_.empty()) ext_focus_stack_.pop_back();
      after_nav();
      focus_.index = back_to;
      return;
    } else if (ext_screen_stack_.size() > 1) {
      ext_screen_stack_.pop_back();
      const int back_to = ext_focus_stack_.empty() ? 0 : ext_focus_stack_.back();
      if (!ext_focus_stack_.empty()) ext_focus_stack_.pop_back();
      after_nav();
      focus_.index = back_to;  // after_nav resets focus; land back on the row we came from
      return;
    } else if (!nav_.pop()) {
      nav_.reset(ScreenId::Home);
    }
    after_nav();
    return;
  }
  if (nav_.current() == ScreenId::ExtAppText) return;
  if (ext_app_index_ < 0 || ext_app_index_ >= static_cast<int>(ext_apps_.size())) return;
  const ExtApp& app = ext_apps_[static_cast<size_t>(ext_app_index_)];
  const ExtScreen* sc = ext_find_screen(app, ext_screen_stack_.empty() ? std::string() : ext_screen_stack_.back());
  if (!sc || sc->blocks.empty()) return;
  const int n = static_cast<int>(sc->blocks.size());
  focus_.count = n;
  if (e == InputEvent::Up || e == InputEvent::Down) {
    const int dir = e == InputEvent::Down ? 1 : -1;
    const int prev = focus_.index;
    for (int step = 0; step < n; ++step) {
      focus_.move(dir);
      if (focusable(sc->blocks[static_cast<size_t>(focus_.index)])) break;
    }
    if (focus_.index != prev) mark_content_dirty();
    return;
  }
  if (e != InputEvent::Select || focus_.index >= n) return;
  const ExtBlock& b = sc->blocks[static_cast<size_t>(focus_.index)];
  if (b.kind == ExtBlock::Kind::Row && !b.go.empty() && ext_screen_stack_.size() < 8) {
    ext_focus_stack_.push_back(focus_.index);
    ext_screen_stack_.push_back(b.go);
    focus_.index = 0;
    after_nav();
  } else if (b.kind == ExtBlock::Kind::Text) {
    ext_text_ = b.text;
    ext_text_title_ = sc->title.empty() ? app.name : sc->title;
    ext_focus_stack_.push_back(focus_.index);
    nav_.push(ScreenId::ExtAppText);
    after_nav();
  }
}

}  // namespace pocket
