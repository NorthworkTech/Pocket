#include "pocket/app.hpp"
#include "pocket/json_lite.hpp"
#include "pocket/release.hpp"
#include "pocket/ui.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <cstdio>
#include <cstring>

namespace pocket {

namespace {

constexpr int kOtaSlotBytes = 0x1E0000;
constexpr int kInstallBarY = 560;

std::string size_label(int bytes) {
  char buf[32];
  if (bytes <= 0) return "";
  if (bytes >= 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
  else std::snprintf(buf, sizeof(buf), "%d KB", (bytes + 1023) / 1024);
  return buf;
}

}  // namespace

bool App::parse_firmware_latest(const std::string& json, FirmwareUpdateInfo* out) {
  if (!out || json.empty()) return false;
  JsonValue root;
  if (!json_parse(json, root) || !root.is_object()) return false;
  out->build_id = root.get_string("build_id");
  out->version = root.get_string("version");
  out->url = root.get_string("url");
  out->size_bytes = static_cast<int>(root.get_int("size"));
  out->version_name = root.get_string("version_name");
  if (out->version_name.empty()) {
    const std::string dv = root.get_string("display_version");
    out->version_name = dv.empty() ? std::string("Pocket update") : std::string(kOsName) + " " + dv;
  }
  out->notes.clear();
  if (const JsonValue* notes = root.get("notes"); notes && notes->is_array()) {
    for (const auto& n : notes->items)
      if (n.type == JsonValue::Type::String && !n.str.empty() && out->notes.size() < 6) out->notes.push_back(n.str);
  }
  return !out->url.empty();
}

void App::begin_firmware_update() {
  // 1) Checking…
  ota_installing_ = false;
  ota_progress_ = -1;
  ota_status_ = "Checking for updates\xE2\x80\xA6";
  focus_.index = 0;
  nav_.push(ScreenId::SettingsUpdateProgress);
  after_nav();

  auto result = [&](const char* msg) {
    ota_status_ = msg;
    focus_.index = 0;
    nav_.replace(ScreenId::SettingsUpdateResult);
    after_nav();
  };
  if (!wifi_.connected()) return result("Connect to Wi-Fi to check for updates.");
  const std::string meta = cloud_.firmware_latest_json();
  FirmwareUpdateInfo info;
  if (!parse_firmware_latest(meta, &info)) return result("Couldn't reach the update server. Try again later.");
  if (!info.build_id.empty() && info.build_id == cfg_.fw_build_id) return result("up-to-date");
  if (!ota_) return result("Updates aren't available on this build.");
  if (info.size_bytes > kOtaSlotBytes) return result("This update is too large for Pocket.");

  // 2) Found one — show what it is before anything is downloaded.
  update_info_ = info;
  focus_.index = 0;
  nav_.replace(ScreenId::SettingsUpdateAvailable);
  after_nav();
}

void App::install_firmware_update() {
  const int batt = clock_.battery_percent();
  if (batt >= 0 && batt < 20) {
    ota_status_ = "Charge Pocket to at least 20% to install this update.";
    nav_.replace(ScreenId::SettingsUpdateResult);
    after_nav();
    return;
  }
  // 3) Installing — full refresh so the install screen is crisp, then progress-only repaints.
  ota_installing_ = true;
  ota_progress_ = 0;
  ota_status_.clear();
  nav_.replace(ScreenId::SettingsUpdateProgress);
  after_nav(true);
  int last_shown = 0;
  std::string st;
  const bool ok = ota_->apply_https_ota(update_info_.url, &st, [&](int pct) {
    if (pct < last_shown + 5 && pct < 100) return;  // e-ink: repaint in 5% steps
    last_shown = pct;
    ota_progress_ = pct;
    mark_region_dirty(0, kInstallBarY - 10, kCanvasW, 90);
    if (dirty_ && try_present(false)) dirty_ = false;
  });
  // apply_https_ota restarts the device on success; reaching here means it failed.
  ota_installing_ = false;
  ota_progress_ = -1;
  ota_status_ = st.empty() ? (ok ? "Update installed." : "The update couldn't be installed.") : st;
  nav_.replace(ScreenId::SettingsUpdateResult);
  after_nav();
}

void App::render_update() {
  using ui::Icon;
  draw_status_bar();
  const ScreenId s = nav_.current();

  if (s == ScreenId::SettingsUpdateProgress && ota_installing_) {
    ui::waves(canvas_, kContentTop + 8, 250);
    canvas_.draw_text_centered(kCanvasW / 2, 360, "Installing update", Canvas::TextRole::ScreenTitle, Gray::G0);
    const std::string& name = update_info_.version_name.empty() ? std::string("Pocket update") : update_info_.version_name;
    canvas_.draw_text_centered(kCanvasW / 2, 416, name, Canvas::TextRole::Secondary, Gray::G0);
    canvas_.draw_text_centered(kCanvasW / 2, 460, "Pocket restarts when it's done.", Canvas::TextRole::Caption, Gray::G0);
    canvas_.draw_text_centered(kCanvasW / 2, 486, "Keep it nearby and charged.", Canvas::TextRole::Caption, Gray::G0);
    // Progress (repainted alone as the download advances).
    const int pct = std::clamp(ota_progress_, 0, 100);
    canvas_.fill_rect(0, kInstallBarY - 10, kCanvasW, 90, Gray::G3);
    canvas_.stroke_round_rect(kSideMargin + 20, kInstallBarY, kContentW - 40, 16, 8, Gray::G0, 2, Gray::G3);
    const int fill = (kContentW - 48) * pct / 100;
    if (fill > 0) canvas_.fill_round_rect(kSideMargin + 24, kInstallBarY + 4, std::max(8, fill), 8, 4, Gray::G0);
    char p[16];
    std::snprintf(p, sizeof(p), "%d%%", pct);
    canvas_.draw_text_centered(kCanvasW / 2, kInstallBarY + 30, p, Canvas::TextRole::BodyStrong, Gray::G0);
    return;
  }

  ui::header(canvas_, "Software update", s != ScreenId::SettingsUpdateProgress);

  if (s == ScreenId::SettingsUpdateProgress) {
    ui::empty_state(canvas_, Icon::Update, "Checking for updates\xE2\x80\xA6", "This takes a few seconds.", 380);
    return;
  }

  if (s == ScreenId::SettingsUpdateAvailable) {
    // App-icon tile + name + size, like a phone's update page.
    const int iy = kListTop + 4;
    canvas_.fill_round_rect(kSideMargin, iy, 84, 84, 20, Gray::G0);
    ui::icon(canvas_, Icon::Update, kSideMargin + 42, iy + 42, 46, Gray::G3);
    canvas_.draw_text_fit(kSideMargin + 104, iy + 4, kContentW - 104, update_info_.version_name,
                          Canvas::TextRole::ScreenTitle, Gray::G0);
    std::string meta = kProductName;
    const std::string sz = size_label(update_info_.size_bytes);
    if (!sz.empty()) meta += " \xC2\xB7 " + sz;
    canvas_.draw_text_fit(kSideMargin + 104, iy + 56, kContentW - 104, meta, Canvas::TextRole::Caption, Gray::G0);

    int y = iy + 116;
    ui::section(canvas_, y, "What's new");
    y += 36;
    if (update_info_.notes.empty()) {
      y = canvas_.draw_text_wrapped(kSideMargin, y, kContentW, kWrapGap,
                                    "Improvements and fixes for Pocket.", Canvas::TextRole::Secondary, Gray::G0);
    }
    for (const auto& n : update_info_.notes) {
      if (y > kBottomCtaY - kRowPitch - 60) break;
      canvas_.fill_disc(kSideMargin + 6, y + 15, 3, Gray::G0);
      y = canvas_.draw_text_wrapped(kSideMargin + 22, y, kContentW - 22, 2, n, Canvas::TextRole::Secondary, Gray::G0) + 8;
    }
    focus_.count = 2;
    ui::button(canvas_, kBottomCtaY - kRowPitch, "Install now", focus_.index == 0, Icon::Update, true);
    ui::row(canvas_, kBottomCtaY, ui::Row{"Later", ui::RowKind::Plain}, focus_.index == 1);
    return;
  }

  // Result.
  const bool current = ota_status_ == "up-to-date";
  if (current) {
    ui::empty_state(canvas_, Icon::Check, "Pocket is up to date", kOsDisplayName, 380);
  } else {
    ui::empty_state(canvas_, Icon::Info, "Software update", ota_status_, 380);
  }
  focus_.count = 1;
  ui::button(canvas_, kBottomCtaY, "Done", true);
}

void App::handle_update(InputEvent e) {
  const ScreenId s = nav_.current();
  if (s == ScreenId::SettingsUpdateProgress) return;  // checking / installing owns the screen
  if (s == ScreenId::SettingsUpdateAvailable) {
    focus_.count = 2;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      const int prev = focus_.index;
      focus_.move(e == InputEvent::Down ? 1 : -1);
      if (focus_.index != prev) mark_region_dirty(0, kBottomCtaY - kRowPitch - 6, kCanvasW, kRowPitch + kFocusRowH + 12);
    } else if (e == InputEvent::Select && focus_.index == 0) {
      install_firmware_update();
    } else if (e == InputEvent::Back || e == InputEvent::Select) {
      if (!nav_.pop()) nav_.reset(ScreenId::Home);
      focus_.index = 0;
      after_nav();
    }
    return;
  }
  // Result: Done / Back return to where the check started.
  if (e == InputEvent::Select || e == InputEvent::Back) {
    if (!nav_.pop()) nav_.reset(ScreenId::Home);
    focus_.index = 0;
    after_nav();
  }
}

}  // namespace pocket
