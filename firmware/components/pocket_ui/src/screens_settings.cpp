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
    const char* wifi_val = wifi_.connected() ? cfg_.wifi_ssid.c_str() : "Off";
    const char* cloud_val = cfg_.cloud_status == "active"     ? "Active"
                            : cfg_.cloud_status == "trialing" ? "Trial"
                                                              : "";
    struct Item { const char* label; Icon icon; const char* value; };
    const Item items[9] = {
        {"PIN & security", Icon::Lock, ""},     {"Wi-Fi", Icon::Wifi, wifi_val},
        {"Display", Icon::Display, ""},         {"Sound & mic", Icon::Sound, ""},
        {"Home apps", Icon::Home, ""},          {"Units", Icon::Weather, cfg_.weather_units ? "\xC2\xB0" "C" : "\xC2\xB0" "F"},
        {"Pocket Cloud", Icon::Cloud, cloud_val}, {"Software update", Icon::Update, kOsVersion},
        {"About", Icon::Info, ""},
    };
    focus_.count = 9;
    for (int i = 0; i < 9; ++i) {
      Row r{items[i].label, RowKind::Nav, items[i].value, false, items[i].icon, true};
      ui::row(canvas_, row_y(kListTop, i), r, i == focus_.index);
    }
    return;
  }

  if (s == ScreenId::SettingsAbout) {
    ui::header(canvas_, "About", true);
    char ver[32];
    std::snprintf(ver, sizeof(ver), "%s (%s)", kOsDisplayName, kConsumerVersion);
    int y = kListTop + 4;
    y = ui::kv(canvas_, y, "Model", kProductName);
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
    if (cfg_.screensaver == kScreensaverPlain) saver = "Plain";
    else if (cfg_.screensaver >= 1 && cfg_.screensaver <= kScreensaverCount)
      saver = screensaver_name(cfg_.screensaver - 1);
    else saver = "Rotate";
    const char* bright = cfg_.brightness_percent < 34   ? "Darker"
                         : cfg_.brightness_percent < 67 ? "Normal"
                                                        : "Lighter";
    const std::string lockmsg = cfg_.lock_message.empty() ? "Off" : cfg_.lock_message;
    const Row rows[8] = {
        {"Auto-lock", RowKind::Value, idle},
        {"Battery percentage", RowKind::Toggle, {}, cfg_.show_batt_pct},
        {"Lock message", RowKind::Value, lockmsg},
        {"Screensaver", RowKind::Value, saver},
        {"Contrast", RowKind::Value, bright},
        {"Refresh screen", RowKind::Plain},
        {"Deep refresh", RowKind::Toggle, {}, cfg_.ghosting_frequent},
        {"Back", RowKind::Back},
    };
    focus_.count = 8;
    for (int i = 0; i < 8; ++i) ui::row(canvas_, row_y(kListTop, i), rows[i], i == focus_.index);
    canvas_.draw_text_wrapped(kSideMargin, row_y(kListTop, 8) + 4, kContentW, kWrapGap,
                              "Deep refresh clears ghosting more often. Set a custom lock message in Companion.",
                              Canvas::TextRole::Caption, Gray::G0);
    return;
  }

  if (s == ScreenId::SettingsSound) {
    ui::header(canvas_, "Sound & mic", true);
    char vol[16];
    std::snprintf(vol, sizeof(vol), "%d%%", cfg_.volume_percent);
    const Row rows[3] = {{"Volume", RowKind::Value, vol, false, Icon::Sound, true},
                         {"Mic test", RowKind::Plain, {}, false, Icon::Mic, true},
                         {"Back", RowKind::Back}};
    focus_.count = 3;
    for (int i = 0; i < 3; ++i) ui::row(canvas_, row_y(kListTop, i), rows[i], i == focus_.index);
    const char* mic = ptt_active_                ? "Listening\xE2\x80\xA6 release to finish."
                      : mic_last_level_ < 0      ? "Hold the side button to test the microphone."
                      : mic_last_ok_             ? "Microphone OK \xE2\x80\x94 sound picked up."
                                                 : "No sound detected \xE2\x80\x94 check the microphone.";
    ui::section(canvas_, row_y(kListTop, 3) + 8, "Microphone");
    canvas_.draw_text_wrapped(kSideMargin, row_y(kListTop, 3) + 42, kContentW, kWrapGap, mic,
                              Canvas::TextRole::Secondary, Gray::G0);
    if (mic_last_level_ >= 0 && !ptt_active_) {
      // Level meter from the last test.
      const int my = row_y(kListTop, 3) + 110;
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
    static const int kOrder[] = {0, 1, 2, 3, 4, 5, 8, 6, 7};
    static const char* names[] = {"Notes", "Ledger", "Clock", "Pass", "Weather", "Music", "Reading", "Settings",
                                  "Update"};
    static const Icon icons[] = {Icon::Notes, Icon::Ledger, Icon::Clock,    Icon::Pass,  Icon::Weather,
                                 Icon::Music, Icon::Reading, Icon::Settings, Icon::Update};
    focus_.count = 9;
    for (int i = 0; i < 9; ++i) {
      const HomeApp a = static_cast<HomeApp>(kOrder[i]);
      const bool placeholder = a == HomeApp::Ledger;
      Row r{names[i], (i < 7 && !placeholder) ? RowKind::Toggle : RowKind::Value,
            placeholder ? "Coming soon" : (i < 7 ? (a == HomeApp::Pass ? "" : "") : "Always on"),
            home_app_visible(cfg_, a), icons[i], true};
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
    const int n_actions = n_known > 0 ? 3 : 2;
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
      rows.push_back(Row{net.ssid, cur ? RowKind::Value : RowKind::Plain, cur ? "Connected" : "", false, Icon::Wifi, true});
    }
    rows.push_back(Row{"Add network", RowKind::Nav, {}, false, Icon::Plus, true});
    if (n_known > 0) rows.push_back(Row{"Remove network", RowKind::Nav, {}, false, Icon::Trash, true});
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
    focus_.count = 9;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else if (e == InputEvent::Select) {
      ScreenId dest[] = {ScreenId::SettingsSecurity, ScreenId::SettingsWifi,     ScreenId::SettingsDisplay,
                         ScreenId::SettingsSound,    ScreenId::SettingsHomeApps, ScreenId::SettingsUnits,
                         ScreenId::SettingsCloud,    ScreenId::SettingsUpdate,   ScreenId::SettingsAbout};
      if (dest[focus_.index] == ScreenId::SettingsWifi) wifi_ui_page_ = 0;
      nav_.push(dest[focus_.index]);
      after_nav();
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
    static const int kOrder[] = {0, 1, 2, 3, 4, 5, 8, 6, 7};
    focus_.count = 9;
    if (e == InputEvent::Up || e == InputEvent::Down) {
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else if (e == InputEvent::Select) {
      // Settings + Update (last two rows) always on
      if (focus_.index < 7 && static_cast<HomeApp>(kOrder[focus_.index]) != HomeApp::Ledger) {
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

    const int n_actions = n_known > 0 ? 3 : 2;
    focus_.count = std::max(1, n_known + n_actions);
    if (e == InputEvent::Up || e == InputEvent::Down) {
      settings_move_focus(e == InputEvent::Down ? 1 : -1);
    } else if (e == InputEvent::Select) {
      if (focus_.index < n_known) {
        const WifiKnownNetwork net = cfg_.wifi_known[static_cast<size_t>(focus_.index)];  // copy: upsert reorders
        play_sound(SoundId::Click);
        error_msg_ = "Connecting…";
        error_until_ms_ = now_ms_ + 2000;
        mark_content_dirty();
        wifi_sta_connecting_ = true;
        const bool ok = connect_and_remember(net.ssid, net.password);
        wifi_sta_connecting_ = false;
        if (ok) {
          play_sound(SoundId::Success);
          error_msg_.clear();
        } else {
          play_sound(SoundId::Attention);
          error_msg_ = "Couldn't join that network.";
          error_until_ms_ = now_ms_ + 3000;
        }
        mark_content_dirty();
      } else if (focus_.index == n_known) {
        // Add network…
        wifi_ui_page_ = 1;
        focus_.index = 0;
        mark_content_dirty();
      } else if (n_known > 0 && focus_.index == n_known + 1) {
        wifi_ui_page_ = 2;
        focus_.index = 0;
        mark_content_dirty();
      } else {
        wifi_ui_page_ = 0;
        nav_.return_to(ScreenId::SettingsRoot);
        after_nav();
      }
    }
    return;
  }

  if (s == ScreenId::SettingsDisplay) {
    focus_.count = 8;
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
        // Rotate → each piece → Plain → Rotate.
        if (cfg_.screensaver == kScreensaverPlain) cfg_.screensaver = 0;
        else if (cfg_.screensaver >= kScreensaverCount) cfg_.screensaver = kScreensaverPlain;
        else ++cfg_.screensaver;
        store_.save(cfg_);
        mark_content_dirty();
      } else if (focus_.index == 4) {
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
      } else if (focus_.index == 5) {
        redraw(true);
      } else if (focus_.index == 6) {
        cfg_.ghosting_frequent = !cfg_.ghosting_frequent;
        refresh_.set_ghosting_budget(cfg_.ghosting_frequent ? RefreshPolicy::kGhostingNFrequent
                                                             : RefreshPolicy::kGhostingN);
        store_.save(cfg_);
        redraw(true);  // full refresh now also clears any ghosting already on screen
      } else if (focus_.index == 7) {
        nav_.return_to(ScreenId::SettingsRoot);
        after_nav();
      }
    }
    return;
  }

  if (s == ScreenId::SettingsSound) {
    focus_.count = 3;
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
      } else if (focus_.index == 2) {
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
