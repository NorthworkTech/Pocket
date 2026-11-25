#include "pocket/app.hpp"
#include "pocket/ui.hpp"
#include "pocket/release.hpp"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace pocket {

namespace {

constexpr int kTitleY = kContentTop;

struct RootEntry {
  const char* label;    // nullptr = section header
  const char* section;  // section title when label == nullptr
  ui::Icon icon;
  ScreenId dest;
};

/** Settings home, grouped. Order here = focus order. */
const std::vector<RootEntry>& settings_root_entries() {
  using ui::Icon;
  static const std::vector<RootEntry> k = {
      {nullptr, "CONNECTIONS", Icon::Home, ScreenId::SettingsRoot},
      {"Wi-Fi", nullptr, Icon::Wifi, ScreenId::SettingsWifi},
      {"Pocket Cloud", nullptr, Icon::Cloud, ScreenId::SettingsCloud},
      {nullptr, "PERSONALIZE", Icon::Home, ScreenId::SettingsRoot},
      {"Display", nullptr, Icon::Display, ScreenId::SettingsDisplay},
      {"Sound & quiet hours", nullptr, Icon::Sound, ScreenId::SettingsSound},
      {"Home apps", nullptr, Icon::Home, ScreenId::SettingsHomeApps},
      {nullptr, "DEVICE", Icon::Home, ScreenId::SettingsRoot},
      {"PIN & security", nullptr, Icon::Lock, ScreenId::SettingsSecurity},
      {"Battery & storage", nullptr, Icon::Battery, ScreenId::SettingsStorage},
      {"Software update", nullptr, Icon::Update, ScreenId::SettingsUpdate},
      {"About", nullptr, Icon::Info, ScreenId::SettingsAbout},
      {"Advanced", nullptr, Icon::Settings, ScreenId::SettingsAdvanced},
  };
  return k;
}


}  // namespace

int App::settings_focus_list_top() const {
  switch (nav_.current()) {
    case ScreenId::SettingsAbout:
      return 560;
    case ScreenId::SettingsHomeApps:
      return kListTop + 28;
    case ScreenId::SettingsUpdate:
      return kListTop + 72 + 2 * kBodyLinePitch;
    case ScreenId::SettingsCloud:
      return kListTop + 180;
    case ScreenId::SettingsWifi:
      if (wifi_ui_page_ == 1) return kListTop + 240;
      if (wifi_ui_page_ == 2) return below_title(kListTop) - 8;
      // Empty state inserts a wrapped hint (+kRowPitch+16) before Add/Back.
      // Page 0 rows sit under the connection status card (kListTop … +64).
      return (cfg_.wifi_known.empty() ? kListTop + 80 + kRowPitch + 16 : kListTop + 80);
    default:
      return kListTop;
  }
}

void App::mark_list_focus_dirty(int list_top, int prev, int next) {
  ++list_focus_spin_count_;
  const int n = focus_.count;
  // End-wrap spans the whole list — content band avoids a tall ghosting rect.
  if (n > 1 && ((prev == 0 && next == n - 1) || (prev == n - 1 && next == 0))) {
    mark_content_dirty();
    return;
  }
  // Periodic content-band clears residual focus ghosts without slowing LUTs.
  if (list_focus_spin_count_ % 4 == 0) {
    mark_content_dirty();
    return;
  }
  // Coalesced jump across many rows → content band (same as Home diagonal wrap).
  if (std::abs(prev - next) > 2) {
    mark_content_dirty();
    return;
  }
  const int i0 = std::min(prev, next);
  const int i1 = std::max(prev, next);
  const int y = std::max(0, row_y(list_top, i0) - 4);
  const int bottom = std::min(kCanvasH, row_y(list_top, i1) + kRowPitch);  // incl. the hairline under the row
  mark_region_dirty(0, y, kCanvasW, bottom - y);
}

void App::settings_move_focus(int delta) {
  const int prev = focus_.index;
  focus_.move(delta);
  if (focus_.index == prev) return;
  if (nav_.current() == ScreenId::SettingsWifi && wifi_ui_page_ == 0 && focus_.count > 8) {
    mark_content_dirty();  // windowed list — rows shift when it scrolls
    return;
  }
  mark_list_focus_dirty(settings_focus_list_top(), prev, focus_.index);
}


void App::render_settings() {
  draw_status_bar();
  const ScreenId s = nav_.current();
  using ui::Icon;
  using ui::Row;
  using ui::RowKind;

  if (s == ScreenId::SettingsRoot) {
    ui::header(canvas_, "Settings");
    const std::string wifi_val = wifi_.connected() ? cfg_.wifi_ssid : std::string("Off");
    const char* cloud_val = cfg_.cloud_status == "active"     ? "Active"
                            : cfg_.cloud_status == "trialing" ? "Trial"
                                                              : "";
    char batt[16] = "";
    {
      const int b = clock_.battery_percent();
      if (b >= 0) std::snprintf(batt, sizeof(batt), "%d%%", b);
    }
    const auto& entries = settings_root_entries();
    const std::string values[] = {wifi_val, cloud_val, "", "", "", "", batt, kOsVersion, "", ""};
    // Lay out sections + rows, then scroll so the focused row stays on screen.
    std::vector<int> ys;
    int y = kListTop, focus_y = kListTop, item = 0;
    for (const auto& en : entries) {
      if (!en.label) {  // section
        ys.push_back(y);
        y += 34;
        continue;
      }
      ys.push_back(y);
      if (item == focus_.index) focus_y = y;
      ++item;
      y += kRowPitch;
    }
    focus_.count = item;
    const int bottom = kCanvasH - 12;
    const int offset = std::max(0, focus_y + kRowPitch + 20 - bottom);
    item = 0;
    for (size_t k = 0; k < entries.size(); ++k) {
      const int ey = ys[k] - offset;
      const auto& en = entries[k];
      if (!en.label) {
        if (ey >= kListTop - 2 && ey + 30 <= bottom) ui::section(canvas_, ey, en.section);
        continue;
      }
      if (ey >= kListTop - 2 && ey + kFocusRowH <= bottom) {
        Row r{en.label, RowKind::Nav, values[item], false, en.icon, true};
        ui::row(canvas_, ey, r, item == focus_.index);
      }
      ++item;
    }
    if (offset > 0) ui::scrollbar(canvas_, kListTop, bottom - kListTop, offset, bottom - kListTop, y - kListTop);
    return;
  }

  if (s == ScreenId::SettingsStorage) {
    ui::header(canvas_, "Battery & storage", true);
    int y = kListTop + 4;
    const int b = clock_.battery_percent();
    char buf[64];
    if (b >= 0) std::snprintf(buf, sizeof(buf), "%d%%", b);
    y = ui::kv(canvas_, y, "Battery", b >= 0 ? std::string(buf) : std::string("Unknown"));
    const bool sd = sd_present();
    auto gb = [](uint64_t bytes) {
      char t[32];
      if (bytes >= (1ull << 30)) std::snprintf(t, sizeof(t), "%.1f GB", bytes / double(1ull << 30));
      else std::snprintf(t, sizeof(t), "%u MB", static_cast<unsigned>(bytes >> 20));
      return std::string(t);
    };
    std::string card = "Not inserted";
    if (sd && storage_) {
      const std::string root = storage_->music_on_sd() ? storage_->music_root() : storage_->book_root();
      card = "In use \xC2\xB7 " + gb(storage_->free_bytes(root)) + " free";
    }
    y = ui::kv(canvas_, y, "microSD card", card);
    if (storage_ && !storage_->data_root().empty()) {
      y = ui::kv(canvas_, y, sd ? "Notes & data" : "Internal storage",
                 gb(storage_->free_bytes(storage_->data_root())) + " free");
    }
    ui::section(canvas_, y + 6, "On Pocket");
    y += 40;
    y = ui::kv(canvas_, y, "Books", std::to_string(books_.size()));
    y = ui::kv(canvas_, y, "Music", std::to_string(music_tracks_.size()) + (music_tracks_.size() == 1 ? " track" : " tracks"));
    y = ui::kv(canvas_, y, "Notes", std::to_string(data_.notes.size()));
    y = ui::kv(canvas_, y, "Lists", std::to_string(data_.lists.size()));
    canvas_.draw_text_wrapped(kSideMargin, y + 12, kContentW, kWrapGap,
                              sd ? "With a microSD card, Companion lets you upload music up to 80 MB per track."
                                 : "Add a microSD card for more books and music \xE2\x80\x94 Companion raises its upload limits automatically.",
                              Canvas::TextRole::Caption, Gray::G0);
    focus_.count = 1;
    ui::row(canvas_, kBottomCtaY, Row{"Back", RowKind::Back}, true);
    return;
  }

  if (s == ScreenId::SettingsAdvanced) {
    ui::header(canvas_, "Advanced", true);
    const Row rows[3] = {{"Refresh screen now", RowKind::Plain, {}, false, Icon::Display, true},
                         {"Deep refresh more often", RowKind::Toggle, {}, cfg_.ghosting_frequent},
                         {"Back", RowKind::Back}};
    focus_.count = 3;
    for (int i = 0; i < 3; ++i) ui::row(canvas_, row_y(kListTop, i), rows[i], i == focus_.index);
    canvas_.draw_text_wrapped(kSideMargin, row_y(kListTop, 3) + 4, kContentW, kWrapGap,
                              "Pocket already cleans the screen on every screen change and when you pause. Turn this "
                              "on only if you still notice faint ghosting.",
                              Canvas::TextRole::Caption, Gray::G0);
    return;
  }

  if (s == ScreenId::SettingsAbout) {
    ui::header(canvas_, "About", true);
    char ver[32];
    std::snprintf(ver, sizeof(ver), "%s (%s)", kOsDisplayName, kConsumerVersion);
    int y = kListTop + 4;
    y = ui::kv(canvas_, y, "Model", kProductName);
    y = ui::kv(canvas_, y, "Made by", "Northwork");
    y = ui::kv(canvas_, y, "Name", cfg_.device_name);
    y = ui::kv(canvas_, y, "Version", ver);
    y = ui::kv(canvas_, y, "Pocket Cloud", cfg_.cloud_entitled ? "Subscribed" : "Not subscribed");
    y = ui::kv(canvas_, y, "Companion", cfg_.companion_linked ? "Linked" : "Not linked");
    ui::section(canvas_, y + 2, "What's new");
    canvas_.draw_text_wrapped(kSideMargin, y + 34, kContentW, kWrapGap,
                              kReleaseNoteCount > 0 ? kReleaseNotes[0] : "",
                              Canvas::TextRole::Secondary, Gray::G0);
    focus_.count = 3;
    const Row acts[3] = {{"Controls tips", RowKind::Nav},
                         {reset_armed_ ? "Press again to erase everything" : "Reset Pocket\xE2\x80\xA6",
                          reset_armed_ ? RowKind::Action : RowKind::Plain},
                         {"Back", RowKind::Back}};
    for (int i = 0; i < 3; ++i) ui::row(canvas_, row_y(560, i), acts[i], i == focus_.index);
    return;
  }

  if (s == ScreenId::SettingsCloud) {
    ui::header(canvas_, "Pocket Cloud", true);
    const char* status = "Not subscribed";
    if (cfg_.cloud_status == "trialing") status = "Trial";
    else if (cfg_.cloud_status == "active") status = "Subscribed";
    else if (cfg_.cloud_status == "past_due") status = "Payment issue";
    int y = ui::kv(canvas_, kListTop + 4, "Plan", status);
    y = ui::kv(canvas_, y, "Companion", cfg_.companion_linked ? "Linked" : "Not linked");
    canvas_.draw_text_wrapped(kSideMargin, y, kContentW, kWrapGap,
                              wifi_.connected() ? "Sync Notes, Lists, Music and Reading with your phone."
                                                : "Offline \xE2\x80\x94 everything stays on Pocket until Wi-Fi returns.",
                              Canvas::TextRole::Secondary, Gray::G0);
    const int top = kListTop + 180;
    focus_.count = 3;
    const Row acts[3] = {{"Start free trial", RowKind::Action},
                         {"Subscribe", RowKind::Value, "$3.99 / month"},
                         {"Link Companion", RowKind::Nav, {}, false, Icon::Phone, true}};
    for (int i = 0; i < 3; ++i) ui::row(canvas_, row_y(top, i), acts[i], i == focus_.index);
    canvas_.draw_text_wrapped(kSideMargin, row_y(top, 3) + 8, kContentW, kWrapGap,
                              "7-day free trial. Cancel anytime in Companion. Link shows a QR for the Pocket app.",
                              Canvas::TextRole::Caption, Gray::G0);
    if (now_ms_ < error_until_ms_ && !error_msg_.empty()) ui::toast(canvas_, error_msg_);
    return;
  }

  if (s == ScreenId::SettingsDisplay) {
    ui::header(canvas_, "Display", true);
    char idle[16];
    if (cfg_.idle_lock_s >= 60) std::snprintf(idle, sizeof(idle), "%d min", cfg_.idle_lock_s / 60);
    else std::snprintf(idle, sizeof(idle), "%d sec", cfg_.idle_lock_s);
    std::string saver;
    if (cfg_.screensaver == kScreensaverWallpaper && wall_selected()) saver = wall_selected()->name;
    else if (cfg_.screensaver == kScreensaverPlain) saver = "Plain";
    else if (cfg_.screensaver >= 1 && cfg_.screensaver <= kScreensaverCount)
      saver = screensaver_name(cfg_.screensaver - 1);
    else saver = "Rotate";
    const char* bright = cfg_.brightness_percent < 34   ? "Darker"
                         : cfg_.brightness_percent < 67 ? "Normal"
                                                        : "Lighter";
    const std::string lockmsg = cfg_.lock_message.empty() ? "Off" : cfg_.lock_message;
    const Row rows[7] = {
        {"Auto-lock", RowKind::Value, idle},
        {"Battery percentage", RowKind::Toggle, {}, cfg_.show_batt_pct},
        {"Lock message", RowKind::Value, lockmsg},
        {"Lock screen info", RowKind::Toggle, {}, cfg_.lock_info},
        {"Screensaver", RowKind::Value, saver},
        {"Contrast", RowKind::Value, bright},
        {"Back", RowKind::Back},
    };
    focus_.count = 7;
    for (int i = 0; i < 7; ++i) ui::row(canvas_, row_y(kListTop, i), rows[i], i == focus_.index);
    canvas_.draw_text_wrapped(kSideMargin, row_y(kListTop, 7) + 4, kContentW, kWrapGap,
                              "Lock screen info shows the date, your next alarm and the weather under the "
                              "screensaver. Set a custom lock message in Companion.",
                              Canvas::TextRole::Caption, Gray::G0);
    return;
  }

  if (s == ScreenId::SettingsSound) {
    ui::header(canvas_, "Sound & mic", true);
    char vol[16];
    std::snprintf(vol, sizeof(vol), "%d%%", cfg_.volume_percent);
    auto hm = [&](int mins) {
      char b[16];
      const int h = mins / 60;
      if (cfg_.time_format == 24) std::snprintf(b, sizeof(b), "%02d:00", h);
      else std::snprintf(b, sizeof(b), "%d %s", h % 12 == 0 ? 12 : h % 12, h < 12 ? "AM" : "PM");
      return std::string(b);
    };
    const std::string qs = hm(cfg_.quiet_start_min), qe = hm(cfg_.quiet_end_min);
    const Row rows[6] = {{"Volume", RowKind::Value, vol, false, Icon::Sound, true},
                         {"Quiet hours", RowKind::Toggle, {}, cfg_.quiet_enabled},
                         {"Quiet from", RowKind::Value, qs},
                         {"Quiet until", RowKind::Value, qe},
                         {"Mic test", RowKind::Plain, {}, false, Icon::Mic, true},
                         {"Back", RowKind::Back}};
    focus_.count = 6;
    for (int i = 0; i < 6; ++i) ui::row(canvas_, row_y(kListTop, i), rows[i], i == focus_.index);
    const char* mic = ptt_active_                ? "Listening\xE2\x80\xA6 release to finish."
                      : mic_last_level_ < 0      ? "Hold the back button to test the microphone."
                      : mic_last_ok_             ? "Microphone OK \xE2\x80\x94 sound picked up."
                                                 : "No sound detected \xE2\x80\x94 check the microphone.";
    ui::section(canvas_, row_y(kListTop, 6) + 8, "Microphone");
    canvas_.draw_text_wrapped(kSideMargin, row_y(kListTop, 6) + 42, kContentW, kWrapGap, mic,
                              Canvas::TextRole::Secondary, Gray::G0);
    if (mic_last_level_ >= 0 && !ptt_active_) {
      // Level meter from the last test.
      const int my = row_y(kListTop, 6) + 110;
      canvas_.stroke_round_rect(kSideMargin, my, kContentW, 14, 7, Gray::G0, 2, Gray::G3);
      const int fill = (kContentW - 8) * std::clamp(mic_last_level_, 0, 100) / 100;
      if (fill > 0) canvas_.fill_round_rect(kSideMargin + 4, my + 4, std::max(6, fill), 6, 3, Gray::G0);
    }
    return;
  }

  if (s == ScreenId::SettingsUnits) {
    ui::header(canvas_, "Units", true);
    focus_.count = 2;
    const char* names[2] = {"Fahrenheit (\xC2\xB0" "F)", "Celsius (\xC2\xB0" "C)"};
    for (int i = 0; i < 2; ++i) {
      Row r{names[i], RowKind::Radio, {}, cfg_.weather_units == i};
      ui::row(canvas_, row_y(kListTop, i), r, i == focus_.index);
    }
    canvas_.draw_text_wrapped(kSideMargin, row_y(kListTop, 2) + 4, kContentW, kWrapGap,
                              "Used by Weather. Back to return.", Canvas::TextRole::Caption, Gray::G0);
    return;
  }

  if (s == ScreenId::SettingsHomeApps) {
    ui::header(canvas_, "Home apps", true);
    canvas_.draw_text(kSideMargin, kListTop - 6, "Choose what appears on Home.",
                      Canvas::TextRole::Caption, Gray::G0);
    static const int kOrder[] = {9, 0, 2, 4, 5, 8, 3, 6, 7};
    static const char* names[] = {"Today", "Notes", "Clock", "Weather", "Music", "Reading", "Pass", "Settings",
                                  "Update"};
    static const Icon icons[] = {Icon::Calendar, Icon::Notes,   Icon::Clock,    Icon::Weather, Icon::Music,
                                 Icon::Reading,  Icon::Pass,    Icon::Settings, Icon::Update};
    focus_.count = 9;
    for (int i = 0; i < 9; ++i) {
      const HomeApp a = static_cast<HomeApp>(kOrder[i]);
      Row r{names[i], i < 7 ? RowKind::Toggle : RowKind::Value, i < 7 ? "" : "Always on", home_app_visible(cfg_, a),
            icons[i], true};
      ui::row(canvas_, row_y(kListTop + 28, i), r, i == focus_.index);
    }
    return;
  }

  if (s == ScreenId::SettingsUpdate) {
    ui::header(canvas_, "Software update", true);
    int y = ui::kv(canvas_, kListTop + 4, "Installed", kOsDisplayName);
    y = ui::kv(canvas_, y, "Channel", cfg_.fw_channel == "stable" ? "Stable" : cfg_.fw_channel);
    (void)y;
    canvas_.draw_text_fit(kSideMargin, kListTop + 106, kContentW,
                          wifi_.connected() ? "Signed updates install over Wi-Fi." : "Connect to Wi-Fi to check.",
                          Canvas::TextRole::Caption, Gray::G0);
    focus_.count = 2;
    const Row rows[2] = {{"Check for update", RowKind::Action, {}, false, Icon::Update, true},
                         {"Back", RowKind::Back}};
    for (int i = 0; i < 2; ++i) ui::row(canvas_, row_y(kListTop + 72 + 2 * kBodyLinePitch, i), rows[i], i == focus_.index);
    return;
  }

  if (s == ScreenId::SettingsWifi) {
    ui::header(canvas_, "Wi-Fi", true);

    if (wifi_ui_page_ == 1) {
      canvas_.draw_text_wrapped(kSideMargin, kListTop, kContentW, kWrapGap,
                                "Add a network from your phone \xE2\x80\x94 your Personal Hotspot or another Wi-Fi.",
                                Canvas::TextRole::Body, Gray::G0);
      canvas_.draw_text_wrapped(kSideMargin, kListTop + 120, kContentW, kWrapGap,
                                "Pocket opens a short setup network and stays unlocked until you finish or cancel.",
                                Canvas::TextRole::Secondary, Gray::G0);
      focus_.count = 3;
      const Row acts[3] = {{"Personal Hotspot", RowKind::Nav, {}, false, Icon::Phone, true},
                           {"Another Wi-Fi", RowKind::Nav, {}, false, Icon::Wifi, true},
                           {"Back", RowKind::Back}};
      for (int i = 0; i < 3; ++i) ui::row(canvas_, row_y(kListTop + 240, i), acts[i], i == focus_.index);
      return;
    }

    if (wifi_ui_page_ == 3 && wifi_detail_index_ >= 0 &&
        wifi_detail_index_ < static_cast<int>(cfg_.wifi_known.size())) {
      const auto& net = cfg_.wifi_known[static_cast<size_t>(wifi_detail_index_)];
      const bool cur = wifi_.connected() && net.ssid == cfg_.wifi_ssid;
      ui::icon(canvas_, Icon::Wifi, kSideMargin + 14, kListTop + 22, 28, Gray::G0);
      canvas_.draw_text_fit(kSideMargin + 42, kListTop + 5, kContentW - 42, net.ssid, Canvas::TextRole::BodyStrong,
                            Gray::G0);
      canvas_.draw_text(kSideMargin + 42, kListTop + 36, cur ? "Connected" : "Saved", Canvas::TextRole::Caption,
                        Gray::G0);
      focus_.count = 3;
      const Row acts[3] = {{cur ? "Reconnect" : "Connect", RowKind::Action, {}, false, Icon::Wifi, true},
                           {"Forget this network", RowKind::Plain, {}, false, Icon::Trash, true},
                           {"Back", RowKind::Back}};
      for (int i = 0; i < 3; ++i) ui::row(canvas_, row_y(kListTop + 80, i), acts[i], i == focus_.index);
      if (now_ms_ < error_until_ms_ && !error_msg_.empty()) ui::toast(canvas_, error_msg_);
      return;
    }

    if (wifi_ui_page_ == 2) {
      ui::section(canvas_, kListTop, "Remove a saved network");
      const int n_known = static_cast<int>(cfg_.wifi_known.size());
      focus_.count = std::max(1, n_known + 1);
      int y = below_title(kListTop) - 8;
      for (int i = 0; i < n_known; ++i) {
        Row r{cfg_.wifi_known[static_cast<size_t>(i)].ssid, RowKind::Plain, {}, false, Icon::Trash, true};
        ui::row(canvas_, y, r, i == focus_.index);
        y += kRowPitch;
      }
      ui::row(canvas_, y, Row{"Back", RowKind::Back}, focus_.index == n_known);
      return;
    }

    const bool online = wifi_.connected();
    // Status card.
    {
      const int cy = kListTop + 22;
      ui::icon(canvas_, Icon::Wifi, kSideMargin + 14, cy, 28, Gray::G0);
      const char* st = online ? "Connected" : (wifi_sta_connecting_ ? "Connecting\xE2\x80\xA6" : "Not connected");
      canvas_.draw_text(kSideMargin + 42, cy - 17, st, Canvas::TextRole::BodyStrong, Gray::G0);
      if (online && !cfg_.wifi_ssid.empty()) {
        canvas_.draw_text_fit(kSideMargin + 42, cy + 14, kContentW - 42, cfg_.wifi_ssid, Canvas::TextRole::Caption,
                              Gray::G0);
      }
    }
    const int n_known = static_cast<int>(cfg_.wifi_known.size());
    const int n_actions = 2;  // Add network · Back
    focus_.count = std::max(1, n_known + n_actions);
    int y = kListTop + 80;
    if (n_known == 0) {
      canvas_.draw_text_wrapped(kSideMargin, y, kContentW, kWrapGap,
                                "No saved networks yet. Add one from the Pocket app on your phone.",
                                Canvas::TextRole::Secondary, Gray::G0);
      y += kRowPitch + 16;
    }
    // Known networks + actions scroll as one window (8 saved networks overflowed the panel).
    std::vector<Row> rows;
    for (int i = 0; i < n_known; ++i) {
      const auto& net = cfg_.wifi_known[static_cast<size_t>(i)];
      const bool cur = online && net.ssid == cfg_.wifi_ssid;
      rows.push_back(Row{net.ssid, cur ? RowKind::Value : RowKind::Nav, cur ? "Connected" : "", false, Icon::Wifi, true});
    }
    rows.push_back(Row{"Add network", RowKind::Nav, {}, false, Icon::Plus, true});
    rows.push_back(Row{"Back", RowKind::Back});
    const int total = static_cast<int>(rows.size());
    const int visible = std::max(1, (kCanvasH - 16 - y) / kRowPitch);
    const int start = ui::window_start(focus_.index, total, visible);
    for (int k = 0; k < visible && start + k < total; ++k) {
      ui::row(canvas_, y + k * kRowPitch, rows[static_cast<size_t>(start + k)], start + k == focus_.index);
    }
    ui::scrollbar(canvas_, y, visible * kRowPitch - 8, start, visible, total);
    if (now_ms_ < error_until_ms_ && !error_msg_.empty()) ui::toast(canvas_, error_msg_);
    return;
  }

  if (s == ScreenId::SettingsSecurity) {
    ui::header(canvas_, "PIN & security", true);
    focus_.count = 3;
    const Row rows[3] = {{"Change PIN", RowKind::Nav, {}, false, Icon::Lock, true},
                         {"Lock now", RowKind::Action},
                         {"Back", RowKind::Back}};
    for (int i = 0; i < 3; ++i) ui::row(canvas_, row_y(kListTop, i), rows[i], i == focus_.index);
    return;
  }

  ui::header(canvas_, "Settings");
}

void App::handle_settings(InputEvent e) {
  ScreenId s = nav_.current();

  if (e == InputEvent::Back) {
    if (s == ScreenId::SettingsWifi && wifi_ui_page_ != 0) {
      wifi_ui_page_ = 0;
      focus_.index = 0;
      mark_content_dirty();
      return;
    }
    if (s == ScreenId::SettingsRoot) {
      nav_.pop();
    } else {
      nav_.return_to(ScreenId::SettingsRoot);
    }
    focus_.index = 0;
    reset_armed_ = false;
    after_nav();
    return;
  }

  if (s == ScreenId::SettingsRoot) {
    std::vector<ScreenId> dest;
    for (const auto& en : settings_root_entries())
      if (en.label) dest.push_back(en.dest);
    focus_.count = static_cast<int>(dest.size());
    if (e == InputEvent::Up || e == InputEvent::Down) {
      const int prev = focus_.index;
      focus_.move(e == InputEvent::Down ? 1 : -1);
      if (focus_.index != prev) mark_content_dirty();  // grouped list may scroll
    } else if (e == InputEvent::Select && focus_.index < focus_.count) {
      if (dest[focus_.index] == ScreenId::SettingsWifi) wifi_ui_page_ = 0;
      nav_.push(dest[focus_.index]);
      after_nav();
    }
    return;
  }

  if (s == ScreenId::SettingsStorage) {
    if (e == InputEvent::Select) {
      nav_.return_to(ScreenId::SettingsRoot);
      after_nav();
    }
    return;
  }

  if (s == ScreenId::SettingsAdvanced) {
    focus_.count = 3;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else if (e == InputEvent::Select) {
      if (focus_.index == 0) {
        want_deep_ = true;
        redraw(true);
      } else if (focus_.index == 1) {
        cfg_.ghosting_frequent = !cfg_.ghosting_frequent;
        refresh_.set_ghosting_budget(cfg_.ghosting_frequent ? RefreshPolicy::kGhostingNFrequent
                                                             : RefreshPolicy::kGhostingN);
        store_.save(cfg_);
        redraw(true);
      } else {
        nav_.return_to(ScreenId::SettingsRoot);
        after_nav();
      }
    }
    return;
  }

  if (s == ScreenId::SettingsUnits && (e == InputEvent::Select || e == InputEvent::Up || e == InputEvent::Down)) {
    focus_.count = 2;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else {
      cfg_.weather_units = static_cast<uint8_t>(focus_.index);
      store_.save(cfg_);
      mark_content_dirty();
    }
    return;
  }

  if (s == ScreenId::SettingsHomeApps) {
    static const int kOrder[] = {9, 0, 2, 4, 5, 8, 3, 6, 7};
    focus_.count = 9;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else if (e == InputEvent::Select) {
      // Settings + Update (last two rows) always on
      if (focus_.index < 7) {
        cfg_.home_visible ^= static_cast<uint16_t>(1u << kOrder[focus_.index]);
        store_.save(cfg_);
        mark_content_dirty();
      }
    }
    return;
  }

  if (s == ScreenId::SettingsSecurity) {
    focus_.count = 3;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else if (e == InputEvent::Select) {
      if (focus_.index == 0) {
        // Change PIN: prove the current PIN first (handle_pin → OnboardingPinSet/Confirm).
        pin_change_ = true;
        pin_entry_.clear();
        pin_pending_.clear();
        pin_spin_count_ = 0;
        pin_ignore_select_until_ms_ = 0;
        pin_enter_empty_slot();
        nav_.push(ScreenId::Pin);
        after_nav();
      } else if (focus_.index == 1) {
        go_lock();
      } else if (focus_.index == 2) {
        nav_.return_to(ScreenId::SettingsRoot);
        after_nav();
      }
    }
    return;
  }

  if (s == ScreenId::SettingsWifi) {
    const int n_known = static_cast<int>(cfg_.wifi_known.size());

    if (wifi_ui_page_ == 1) {
      focus_.count = 3;
      if (e == InputEvent::Up || e == InputEvent::Down) {
        settings_move_focus(e == InputEvent::Down ? 1 : -1);
        return;
      }
      if (e == InputEvent::Select) {
        if (focus_.index == 2) {
          wifi_ui_page_ = 0;
          focus_.index = n_known;  // Add row
          mark_content_dirty();
          return;
        }
        // Hotspot (0) or Another Wi‑Fi (1).
        // Always SoftAP (APSTA when already online) so the phone can finish without
        // the device idle/Power-locking mid-handoff. Companion Cloud queue remains
        // available from the phone/Mac control when Pocket is already online.
        play_sound(SoundId::Click);
        wifi_ui_page_ = 0;
        begin_add_wifi_network();
      }
      return;
    }

    if (wifi_ui_page_ == 2) {
      focus_.count = std::max(1, n_known + 1);
      if (e == InputEvent::Up || e == InputEvent::Down) {
        settings_move_focus(e == InputEvent::Down ? 1 : -1);
        return;
      }
      if (e == InputEvent::Select) {
        if (focus_.index >= n_known) {
          wifi_ui_page_ = 0;
          focus_.index = 0;
          mark_content_dirty();
          return;
        }
        const std::string victim = cfg_.wifi_known[static_cast<size_t>(focus_.index)].ssid;
        wifi_known_forget(cfg_, victim);
        store_.save(cfg_);
        play_sound(SoundId::Click);
        if (cfg_.wifi_known.empty()) {
          wifi_ui_page_ = 0;
          focus_.index = 0;
        } else if (focus_.index >= static_cast<int>(cfg_.wifi_known.size())) {
          focus_.index = static_cast<int>(cfg_.wifi_known.size()) - 1;
        }
        mark_content_dirty();
      }
      return;
    }

    if (wifi_ui_page_ == 3) {
      focus_.count = 3;
      if (wifi_detail_index_ < 0 || wifi_detail_index_ >= n_known) {
        wifi_ui_page_ = 0;
        focus_.index = 0;
        mark_content_dirty();
        return;
      }
      if (e == InputEvent::Up || e == InputEvent::Down) {
        settings_move_focus(e == InputEvent::Down ? 1 : -1);
        return;
      }
      if (e == InputEvent::Back || (e == InputEvent::Select && focus_.index == 2)) {
        wifi_ui_page_ = 0;
        focus_.index = wifi_detail_index_;
        mark_content_dirty();
        return;
      }
      if (e != InputEvent::Select) return;
      if (focus_.index == 1) {
        wifi_known_forget(cfg_, cfg_.wifi_known[static_cast<size_t>(wifi_detail_index_)].ssid);
        store_.save(cfg_);
        play_sound(SoundId::Click);
        wifi_ui_page_ = 0;
        focus_.index = 0;
        mark_content_dirty();
        return;
      }
      const WifiKnownNetwork net = cfg_.wifi_known[static_cast<size_t>(wifi_detail_index_)];  // copy: upsert reorders
      play_sound(SoundId::Click);
      wifi_sta_connecting_ = true;
      const bool ok = connect_and_remember(net.ssid, net.password);
      wifi_sta_connecting_ = false;
      play_sound(ok ? SoundId::Success : SoundId::Attention);
      if (ok) {
        error_msg_.clear();
        wifi_ui_page_ = 0;
        focus_.index = 0;
      } else {
        const std::string why = wifi_.last_error();
        error_msg_ = why.empty() ? std::string("Couldn't join that network.") : why;
        error_until_ms_ = now_ms_ + 4000;
      }
      mark_content_dirty();
      return;
    }

    const int n_actions = 2;  // Add network · Back
    focus_.count = std::max(1, n_known + n_actions);
    if (e == InputEvent::Up || e == InputEvent::Down) {
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else if (e == InputEvent::Select) {
      if (focus_.index < n_known) {
        // A saved network → its page (Connect / Forget).
        wifi_detail_index_ = focus_.index;
        wifi_ui_page_ = 3;
        focus_.index = 0;
        mark_content_dirty();
        return;
      }
      if (focus_.index == n_known) {
        // Add network → straight to phone setup (the phone page offers home Wi-Fi or hotspot).
        play_sound(SoundId::Click);
        wifi_ui_page_ = 0;
        begin_add_wifi_network();
      } else {
        wifi_ui_page_ = 0;
        nav_.return_to(ScreenId::SettingsRoot);
        after_nav();
      }
    }
    return;
  }

  if (s == ScreenId::SettingsDisplay) {
    focus_.count = 7;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else if (e == InputEvent::Select) {
      if (focus_.index == 0) {
        const uint16_t opts[] = {30, 60, 120, 300};
        int cur = 0;
        for (int i = 0; i < 4; ++i)
          if (opts[i] == cfg_.idle_lock_s) cur = i;
        cfg_.idle_lock_s = opts[(cur + 1) % 4];
        store_.save(cfg_);
        mark_content_dirty();
      } else if (focus_.index == 1) {
        cfg_.show_batt_pct = !cfg_.show_batt_pct;
        store_.save(cfg_);
        mark_content_dirty();
      } else if (focus_.index == 2) {
        // Quick on-device toggle: Off ↔ device name. Custom text: Companion.
        cfg_.lock_message = cfg_.lock_message.empty() ? cfg_.device_name : std::string();
        store_.save(cfg_);
        mark_content_dirty();
      } else if (focus_.index == 3) {
        cfg_.lock_info = !cfg_.lock_info;
        store_.save(cfg_);
        mark_content_dirty();
      } else if (focus_.index == 4) {
        // Rotate → each piece → Plain → each Northwork wallpaper → Rotate.
        const auto& walls = wall_idx_.items;
        if (cfg_.screensaver == kScreensaverWallpaper) {
          size_t i = 0;
          while (i < walls.size() && walls[i].id != (wall_selected() ? wall_selected()->id : std::string())) ++i;
          if (i + 1 < walls.size()) {
            wall_idx_.selected = walls[i + 1].id;
          } else {
            cfg_.screensaver = 0;
          }
          wallpapers_save();
        } else if (cfg_.screensaver == kScreensaverPlain) {
          if (!walls.empty()) {
            cfg_.screensaver = kScreensaverWallpaper;
            wall_idx_.selected = walls.front().id;
            wallpapers_save();
          } else {
            cfg_.screensaver = 0;
          }
        } else if (cfg_.screensaver >= kScreensaverCount) {
          cfg_.screensaver = kScreensaverPlain;
        } else {
          ++cfg_.screensaver;
        }
        store_.save(cfg_);
        mark_content_dirty();
      } else if (focus_.index == 5) {
        const uint8_t opts[] = {20, 50, 80};
        int cur = 1;
        for (int i = 0; i < 3; ++i)
          if (opts[i] == cfg_.brightness_percent) cur = i;
        cfg_.brightness_percent = opts[(cur + 1) % 3];
        display_.set_brightness(cfg_.brightness_percent);
        store_.save(cfg_);
        if (cfg_.companion_linked && wifi_.connected()) {
          cloud_.push_device_settings(cfg_.device_id, cfg_.volume_percent, cfg_.brightness_percent);
        }
        redraw(true);  // full refresh so the new contrast is visible immediately
      } else if (focus_.index == 6) {
        nav_.return_to(ScreenId::SettingsRoot);
        after_nav();
      }
    }
    return;
  }

  if (s == ScreenId::SettingsSound) {
    focus_.count = 6;
    if (e == InputEvent::PttStart) {
      ptt_active_ = true;
      mic_capturing_ = true;
      if (audio_) audio_->start_capture();
      mark_content_dirty();
      return;
    }
    if (e == InputEvent::PttStop) {
      ptt_active_ = false;
      mic_capturing_ = false;
      const MicCaptureResult r = audio_ ? audio_->stop_capture() : MicCaptureResult{};
      mic_last_level_ = r.ok ? r.level_percent : -1;
      mic_last_ok_ = r.ok && r.level_percent >= 6;
      mark_content_dirty();
      return;
    }
    if (e == InputEvent::Up || e == InputEvent::Down) {
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else if (e == InputEvent::Select) {
      if (focus_.index == 0) {
        int v = cfg_.volume_percent + 10;
        cfg_.volume_percent = static_cast<uint8_t>(v > 100 ? 0 : v);
        if (audio_) audio_->set_volume(cfg_.volume_percent);
        store_.save(cfg_);
        if (cfg_.companion_linked && wifi_.connected()) {
          cloud_.push_device_settings(cfg_.device_id, cfg_.volume_percent, cfg_.brightness_percent);
        }
        play_sound(SoundId::Click);
        mark_content_dirty();
      } else if (focus_.index == 1) {
        cfg_.quiet_enabled = !cfg_.quiet_enabled;
        store_.save(cfg_);
        mark_content_dirty();
      } else if (focus_.index == 2) {
        static const uint16_t kFrom[] = {20 * 60, 21 * 60, 22 * 60, 23 * 60};
        int cur = 0;
        for (int i = 0; i < 4; ++i)
          if (kFrom[i] == cfg_.quiet_start_min) cur = i;
        cfg_.quiet_start_min = kFrom[(cur + 1) % 4];
        store_.save(cfg_);
        mark_content_dirty();
      } else if (focus_.index == 3) {
        static const uint16_t kUntil[] = {5 * 60, 6 * 60, 7 * 60, 8 * 60, 9 * 60};
        int cur = 0;
        for (int i = 0; i < 5; ++i)
          if (kUntil[i] == cfg_.quiet_end_min) cur = i;
        cfg_.quiet_end_min = kUntil[(cur + 1) % 5];
        store_.save(cfg_);
        mark_content_dirty();
      } else if (focus_.index == 5) {
        nav_.return_to(ScreenId::SettingsRoot);
        after_nav();
      }
    }
    return;
  }

  if (s == ScreenId::SettingsAbout) {
    focus_.count = 3;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      if (reset_armed_) {
        reset_armed_ = false;
        mark_content_dirty();
      }
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else if (e == InputEvent::Select) {
      if (focus_.index == 0) {
        tips_page_ = 1;
        cfg_.seen_whats_new_build = cfg_.fw_build_id;
        store_.save(cfg_);
        nav_.push(ScreenId::OnboardingDone);
        after_nav();
      } else if (focus_.index == 1) {
        if (!reset_armed_) {
          // First press arms; the row turns into an explicit confirmation.
          reset_armed_ = true;
          play_sound(SoundId::Attention);
          mark_content_dirty();
          return;
        }
        reset_armed_ = false;
        DeviceConfig fresh;
        fresh.device_id = cfg_.device_id;
        cfg_ = fresh;
        store_.save(cfg_);
        // Erase on-device content too — a reset that leaves notes behind isn't a reset.
        data_ = AppData{};
        save_local_notes();
        save_local_lists();
        save_local_passes();
        save_alarms();
        timer_running_ = false;
        nav_.reset(ScreenId::OnboardingWelcome);
        after_nav();
      } else {
        nav_.return_to(ScreenId::SettingsRoot);
        after_nav();
      }
    }
    return;
  }

  if (s == ScreenId::SettingsCloud) {
    focus_.count = 3;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else if (e == InputEvent::Select) {
      if (!wifi_.connected()) {
        error_msg_ = "Connect to Wi‑Fi for Cloud and Companion Link.";
        error_until_ms_ = now_ms_ + 3000;
        mark_content_dirty();
        return;
      }
      if (focus_.index == 2) {
        // Link companion app — mint pair session + show QR
        (void)mint_pair_session();  // Link screen shows the code, or why it couldn't get one
        focus_.index = 0;
        nav_.push(ScreenId::OnboardingCompanionQr);
        after_nav();
      } else {
        // Trial / subscribe — open companion billing via same pair path if unlinked
        mark_content_dirty();
      }
    }
    return;
  }

  if (s == ScreenId::SettingsUpdate) {
    focus_.count = 2;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else if (e == InputEvent::Select) {
      if (focus_.index == 0) begin_firmware_update();
      else {
        nav_.return_to(ScreenId::SettingsRoot);
        after_nav();
      }
    }
    return;
  }


  // Generic: Up/Down focus, Select/Back
  if (e == InputEvent::Up || e == InputEvent::Down) {
    settings_move_focus(e == InputEvent::Down ? 1 : -1);
  } else if (e == InputEvent::Select) {
    nav_.return_to(ScreenId::SettingsRoot);
    after_nav();
  }
}

}  // namespace pocket
