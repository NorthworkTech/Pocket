#include "pocket/app.hpp"
#include "pocket/cloud_client.hpp"
#include "pocket/ui.hpp"
#include <algorithm>
#include <cstdio>
#include <string>

namespace pocket {

static const char* kTimezones[] = {"America/New_York", "America/Chicago", "America/Denver",
                                   "America/Los_Angeles", "America/Phoenix", "UTC", "Europe/London"};
static constexpr int kTzCount = 7;

/** Friendly names for the setup time-zone list (same order as kTimezones). */
static const char* kTzNames[] = {"New York \xC2\xB7 Eastern", "Chicago \xC2\xB7 Central", "Denver \xC2\xB7 Mountain",
                                 "Los Angeles \xC2\xB7 Pacific", "Phoenix \xC2\xB7 Arizona", "UTC", "London"};

/** Title y for onboarding screens that show a step indicator. */
static int title_y(int step) { return step > 0 ? kOnboardingTitleY : kContentTop; }

namespace {

/** Centered QR inside a rounded card with a quiet zone. */
void qr_card(Canvas& c, int y, int size, const std::string& payload) {
  const int pad = 14;
  const int x = (kCanvasW - size) / 2;
  c.stroke_round_rect(x - pad, y - pad, size + 2 * pad, size + 2 * pad, 16, Gray::G0, 2, Gray::G3);
  if (payload.empty() || !c.draw_qr(x, y, size, payload)) {
    ui::icon(c, ui::Icon::Info, kCanvasW / 2, y + size / 2, 40, Gray::G0);
  }
}

void centered(Canvas& c, int y, std::string_view text, Canvas::TextRole role) {
  const int w = std::min(kContentW, c.text_width(text, role));
  c.draw_text_fit((kCanvasW - w) / 2, y, kContentW, text, role, Gray::G0);
}

/** Pocket silhouette for the controls tour; `hl` = 0 dial, 1 back button, 2 power.
 * Real layout: the dial is on the left edge; on the right edge the power button sits
 * just above the back (function) button. Nothing on the top edge. */
void device_diagram(Canvas& c, int hl) {
  const int w = 150, h = 230;
  const int x = (kCanvasW - w) / 2, y = 196;
  c.stroke_round_rect(x, y, w, h, 22, Gray::G0, 3, Gray::G3);
  c.stroke_round_rect(x + 16, y + 18, w - 32, h - 60, 6, Gray::G0, 2, Gray::G3);
  for (int i = 0; i < 4; ++i) c.hline(x + 30, y + 44 + i * 20, w - 60 - (i == 3 ? 30 : 0), Gray::G0);
  // Dial on the left edge.
  const int dx = x - 14, dy = y + 70;
  if (hl == 0) c.fill_round_rect(dx, dy, 16, 64, 6, Gray::G0);
  else c.stroke_round_rect(dx, dy, 16, 64, 6, Gray::G0, 2, Gray::G3);
  for (int i = 0; i < 5; ++i) c.hline(dx + 3, dy + 10 + i * 11, 10, hl == 0 ? Gray::G3 : Gray::G0);
  // Right edge: power on top, back button just below it.
  const int bx = x + w - 2;
  const int py = y + 64, by = y + 116;
  if (hl == 2) c.fill_round_rect(bx, py, 14, 40, 5, Gray::G0);
  else c.stroke_round_rect(bx, py, 14, 40, 5, Gray::G0, 2, Gray::G3);
  if (hl == 1) c.fill_round_rect(bx, by, 14, 40, 5, Gray::G0);
  else c.stroke_round_rect(bx, by, 14, 40, 5, Gray::G0, 2, Gray::G3);
  // Callout arrows pointing at the highlighted control.
  if (hl == 0) {
    ui::icon(c, ui::Icon::ChevronRight, dx - 28, dy + 32, 26, Gray::G0);
    c.thick_line(dx - 64, dy + 32, dx - 22, dy + 32, 3, Gray::G0);
  } else {
    const int cy = hl == 2 ? py + 20 : by + 20;
    ui::icon(c, ui::Icon::ChevronLeft, bx + 42, cy, 26, Gray::G0);
    c.thick_line(bx + 36, cy, bx + 78, cy, 3, Gray::G0);
  }
}

void page_dots(Canvas& c, int page, int n, int y) {
  const int gap = 18;
  const int x0 = kCanvasW / 2 - (n - 1) * gap / 2;
  for (int i = 0; i < n; ++i) {
    if (i == page) c.fill_disc(x0 + i * gap, y, 5, Gray::G0);
    else c.stroke_circle(x0 + i * gap, y, 5, Gray::G0, 2);
  }
}

}  // namespace

void App::render_onboarding() {
  draw_status_bar();
  using ui::Icon;
  using ui::Row;
  using ui::RowKind;
  const ScreenId s = nav_.current();
  const int step = onboarding_step_of(s);
  if (step > 0 && !cfg_.onboarding_complete && !(s == ScreenId::OnboardingDone && tips_page_ > 0))
    ui::steps(canvas_, step, 8);
  const int ty = title_y(step);
  constexpr int kWrapW = kContentW;
  constexpr int kLineGap = kWrapGap;
  auto title = [&](std::string_view t) { canvas_.draw_text_fit(kSideMargin, ty, kContentW, t, Canvas::TextRole::ScreenTitle, Gray::G0); };
  auto lead = [&](int y, std::string_view t) {
    return canvas_.draw_text_wrapped(kSideMargin, y, kWrapW, kLineGap, t, Canvas::TextRole::Secondary, Gray::G0);
  };

  switch (s) {
    case ScreenId::OnboardingWelcome: {
      title("Welcome to Pocket");
      int y = canvas_.draw_text_wrapped(kSideMargin, below_title(ty) - 4, kWrapW, kLineGap,
                                        "Notes, lists, and daily tools \xE2\x80\x94 quiet, focused, always with you.",
                                        Canvas::TextRole::Body, Gray::G0);
      y = lead(y + 8, "Setup takes about three minutes with the Pocket app on your phone.");
      const int art_y = y + 24;
      const int art_h = kBottomCtaY - 24 - art_y;
      if (art_h > 120) {
        draw_screensaver(canvas_, 1, kSideMargin, art_y, kContentW, art_h, 7);
        canvas_.stroke_rect(kSideMargin, art_y, kContentW, art_h, Gray::G0);
        canvas_.stroke_rect(kSideMargin + 1, art_y + 1, kContentW - 2, art_h - 2, Gray::G0);
      }
      focus_.count = 1;
      ui::button(canvas_, kBottomCtaY, "Begin setup", true);
      break;
    }
    case ScreenId::OnboardingCompanionDownload: {
      title("Get the Pocket app");
      const int y = lead(below_title(ty) - 4, "Scan with your phone's camera to install Pocket.");
      const int qr = 200;
      const int qy = y + 30;
      qr_card(canvas_, qy, qr, companion_download_url());
      canvas_.draw_text_centered(kCanvasW / 2, qy + qr + 30, "or open", Canvas::TextRole::Caption, Gray::G0);
      centered(canvas_, qy + qr + 56, companion_display_origin(), Canvas::TextRole::BodyStrong);
      centered(canvas_, qy + qr + 100, "The app is needed to finish setup.", Canvas::TextRole::Caption);
      focus_.count = 1;
      ui::button(canvas_, kBottomCtaY, "Continue", true);
      break;
    }
    case ScreenId::OnboardingSdCard: {
      title("Storage");
      if (sd_formatting_) {
        ui::empty_state(canvas_, Icon::Sd, "Reformatting the card\xE2\x80\xA6",
                        "This can take up to a minute. Keep the card in.", 380);
        focus_.count = 0;
        break;
      }
      if (sd_waiting_eject_) {
        ui::empty_state(canvas_, Icon::Sd, "Remove the card", "Waiting for the card to be ejected\xE2\x80\xA6", 380);
        focus_.count = 1;
        ui::button(canvas_, kBottomCtaY, "Checking\xE2\x80\xA6", true);
        break;
      }
      const char* head = "Card detected";
      const char* body = "Reformat for Pocket music and files, or eject.";
      const char* acts[2] = {"Reformat and continue", "Eject"};
      if (sd_kind_ == SdContentKind::Absent) {
        head = "No card inserted";
        body = "A microSD card adds room for music and books. You can add one later.";
        acts[0] = "Check again";
        acts[1] = "Continue without a card";
      } else if (sd_kind_ == SdContentKind::FirmwareRisk) {
        head = "This card looks modified";
        body = "Reformat before Pocket uses it, or eject to keep its files.";
      } else if (sd_kind_ == SdContentKind::Media) {
        head = "Card has media files";
        body = "Reformat clears the card for Pocket. Eject keeps your files.";
      } else if (sd_kind_ == SdContentKind::Empty) {
        head = "Empty card ready";
        body = "Reformat prepares the card, or eject to leave it unused.";
      }
      ui::empty_state(canvas_, Icon::Sd, head, body, 360);
      focus_.count = 2;
      ui::row(canvas_, kBottomCtaY - kRowPitch, Row{acts[0], RowKind::Action}, focus_.index == 0);
      ui::row(canvas_, kBottomCtaY, Row{acts[1], RowKind::Plain}, focus_.index == 1);
      if (now_ms_ < error_until_ms_ && !error_msg_.empty()) ui::toast(canvas_, error_msg_);
      break;
    }
    case ScreenId::OnboardingWifiList: {
      title("Choose Wi-Fi");
      int y = lead(below_title(ty) - 4, "You'll enter the password in the Pocket app.");
      const int top = y + 16;
      constexpr int kMaxVisible = 7;
      const int shown = std::min(static_cast<int>(wifi_networks_.size()), kMaxVisible);
      focus_.count = shown + 1;
      if (wifi_networks_.empty()) {
        canvas_.draw_text(kSideMargin, top + 8, "No networks found.", Canvas::TextRole::Secondary, Gray::G0);
      }
      for (int i = 0; i < shown; ++i) {
        ui::row(canvas_, top + i * kRowPitch,
                Row{wifi_networks_[static_cast<size_t>(i)], RowKind::Nav, {}, false, Icon::Wifi, true},
                i == focus_.index);
      }
      const int ry = wifi_networks_.empty() ? top + 48 : top + shown * kRowPitch;
      ui::row(canvas_, ry, Row{"Scan again", RowKind::Plain, {}, false, Icon::Update, true}, focus_.index == shown);
      break;
    }
    case ScreenId::OnboardingWifiPassword: {
      if (wifi_ap_ssid_.empty()) wifi_ap_ssid_ = wifi_.provision_ap_ssid();
      if (wifi_ap_pass_.empty()) wifi_ap_pass_ = wifi_.provision_ap_password();
      title(cfg_.onboarding_complete ? "Add Wi-Fi" : "Connect to Wi-Fi");
      int y = lead(below_title(ty) - 4,
                   cfg_.onboarding_complete
                       ? "On your phone, join this network, then pick the Wi-Fi to add."
                       : "On your phone, join this network, then pick your home Wi-Fi.");
      y += 10;
      y = ui::kv(canvas_, y, "Network", wifi_ap_ssid_.empty() ? std::string("Unavailable") : wifi_ap_ssid_);
      y = ui::kv(canvas_, y, "Password", wifi_ap_pass_.empty() ? std::string("\xE2\x80\x94") : wifi_ap_pass_);
      const int qr = 120;
      const int qy = y + 18;
      qr_card(canvas_, qy, qr, softap_portal_url());
      canvas_.draw_text_centered(kCanvasW / 2, qy + qr + 22, "Scan, or open 192.168.4.1", Canvas::TextRole::Caption,
                                 Gray::G0);
      const char* wait = wifi_sta_connecting_ ? "Connecting to Wi-Fi\xE2\x80\xA6" : "Waiting for your phone\xE2\x80\xA6";
      const bool already_online = wifi_.connected();
      focus_.count = already_online ? 2 : 1;
      const int n = focus_.count;
      const int top = kBottomCtaY - (n - 1) * kRowPitch;
      // Status, not a button: what Pocket is waiting for right now.
      ui::icon(canvas_, Icon::Phone, kSideMargin + 12, top - 30, 22, Gray::G0);
      canvas_.draw_text(kSideMargin + 32, top - 44, wait, Canvas::TextRole::BodyStrong, Gray::G0);
      const Row online{"I'm already online", RowKind::Nav, {}, false, Icon::Wifi, true};
      const Row cancel{"Cancel", RowKind::Back};
      if (already_online) ui::row(canvas_, top, online, focus_.index == 0);
      ui::row(canvas_, top + (n - 1) * kRowPitch, cancel, focus_.index == n - 1);
      if (now_ms_ < error_until_ms_ && !error_msg_.empty()) ui::toast(canvas_, error_msg_);
      break;
    }
    case ScreenId::OnboardingWifiConnecting: {
      title("Connecting");
      ui::empty_state(canvas_, Icon::Wifi, cfg_.wifi_ssid.empty() ? "Connecting\xE2\x80\xA6" : cfg_.wifi_ssid,
                      "This takes a few seconds.", 380);
      break;
    }
    case ScreenId::OnboardingCompanionQr: {
      title("Link Pocket");
      if (pair_code_.empty()) {
        // No network calls while drawing: the screen states what failed and how to retry.
        std::string body = pair_error_.empty() ? std::string("Pocket couldn't reach Pocket Cloud.") : pair_error_;
        if (!cfg_.wifi_ssid.empty() && wifi_.connected()) body += " Pocket is on " + cfg_.wifi_ssid + ".";
        ui::empty_state(canvas_, Icon::Cloud, "Couldn't get a pairing code", body, 360);
        focus_.count = 2;
        ui::row(canvas_, kBottomCtaY - kRowPitch, Row{"Try again", RowKind::Action, {}, false, Icon::Update, true},
                focus_.index == 0);
        ui::row(canvas_, kBottomCtaY, Row{"Choose a different Wi-Fi", RowKind::Nav, {}, false, Icon::Wifi, true},
                focus_.index == 1);
        break;
      }
      const int y = lead(below_title(ty) - 4, "In the Pocket app, scan this code to link Pocket to your account.");
      const int qr = 176;
      const int qy = y + 26;
      qr_card(canvas_, qy, qr, pair_code_.empty() ? std::string() : companion_pair_url(pair_code_));
      std::string spaced;
      for (char ch : pair_code_) {
        if (!spaced.empty()) spaced += ' ';
        spaced += ch;
      }
      int cy = qy + qr + 28;
      canvas_.draw_text_centered(kCanvasW / 2, cy, "or enter this code at", Canvas::TextRole::Caption, Gray::G0);
      centered(canvas_, cy + 24, companion_display_origin() + "/link", Canvas::TextRole::Secondary);
      centered(canvas_, cy + 58, spaced.empty() ? std::string("\xE2\x80\x94") : spaced, Canvas::TextRole::WordMark);
      const char* status = pair_status_ == "expired"   ? "Code expired \xE2\x80\x94 get a new one below"
                           : pair_status_ == "claimed" ? "Linked"
                                                       : "Expires in 10 minutes";
      canvas_.draw_text_centered(kCanvasW / 2, cy + 110, status, Canvas::TextRole::Caption, Gray::G0);
      focus_.count = 2;
      ui::row(canvas_, kBottomCtaY - kRowPitch,
              Row{"Waiting for link\xE2\x80\xA6", RowKind::Plain, {}, false, Icon::Phone, true}, focus_.index == 0);
      ui::row(canvas_, kBottomCtaY, Row{"New code", RowKind::Plain, {}, false, Icon::Update, true},
              focus_.index == 1);
      break;
    }
    case ScreenId::OnboardingPinLength: {
      title("Choose a PIN");
      const int y = lead(below_title(ty) - 4, "Unlock Pocket with a short code. You can change it later in Settings.");
      focus_.count = 2;
      ui::row(canvas_, y + 24, Row{"4 digits", RowKind::Value, "Quicker"}, focus_.index == 0);
      ui::row(canvas_, y + 24 + kRowPitch, Row{"6 digits", RowKind::Value, "Stronger"}, focus_.index == 1);
      break;
    }
    case ScreenId::OnboardingPinSet:
    case ScreenId::OnboardingPinConfirm: {
      const bool confirm = s == ScreenId::OnboardingPinConfirm;
      title(confirm ? (cfg_.onboarding_complete ? "Confirm new PIN" : "Confirm your PIN")
                    : (cfg_.onboarding_complete ? "New PIN" : "Create a PIN"));
      lead(ty + 52, confirm ? "Enter the same digits again." : "Turn to pick a digit, press to set it.");
      draw_pin_entry(/*mask_completed=*/false, ty + 140);
      if (now_ms_ < error_until_ms_ && !error_msg_.empty()) ui::toast(canvas_, error_msg_);
      break;
    }
    case ScreenId::OnboardingTimezone: {
      title("Time zone");
      focus_.count = kTzCount + 3;
      constexpr int kTzRowH = 44;
      const int top = ty + 60;
      for (int i = 0; i < kTzCount; ++i) {
        const int ry = top + i * kTzRowH;
        const bool f = i == focus_.index;
        const Gray ink = f ? Gray::G3 : Gray::G0;
        if (f) canvas_.fill_round_rect(kSideMargin, ry, kFocusRowW, kTzRowH - 4, 10, Gray::G0);
        const int cy = ry + (kTzRowH - 4) / 2;
        canvas_.stroke_circle(kSideMargin + 26, cy, 10, ink, 2);
        if (i == onboarding_tz_index_) canvas_.fill_disc(kSideMargin + 26, cy, 5, ink);
        canvas_.draw_text(kSideMargin + 48, cy - canvas_.text_height(Canvas::TextRole::Secondary) / 2, kTzNames[i],
                          Canvas::TextRole::Secondary, ink);
      }
      int ry = top + kTzCount * kTzRowH + 12;
      ui::section(canvas_, ry - 2, "Clock format");
      ry += 30;
      const int seg = (kFocusRowW - 12) / 2;
      const char* fmts[2] = {"12-hour", "24-hour"};
      for (int i = 0; i < 2; ++i) {
        const int x = kSideMargin + i * (seg + 12);
        const bool f = focus_.index == kTzCount + i;
        const bool on = (i == 0) == (cfg_.time_format != 24);
        const Gray ink = f ? Gray::G3 : Gray::G0;
        if (f) canvas_.fill_round_rect(x, ry, seg, 48, 12, Gray::G0);
        else canvas_.stroke_round_rect(x, ry, seg, 48, 12, Gray::G0, on ? 3 : 1, Gray::G3);
        const int lw = canvas_.text_width(fmts[i], Canvas::TextRole::BodyStrong);
        canvas_.draw_text(x + (seg - lw) / 2, ry + 8, fmts[i], Canvas::TextRole::BodyStrong, ink);
      }
      ui::button(canvas_, kBottomCtaY, "Continue", focus_.index == kTzCount + 2);
      break;
    }
    case ScreenId::OnboardingMicTest: {
      title("Voice");
      lead(below_title(ty) - 4, "Hold the back button and say a few words to check the microphone.");
      const char* head = ptt_active_ ? "Listening\xE2\x80\xA6" : (mic_result_.empty() ? "Hold the back button" : "Mic check");
      ui::empty_state(canvas_, Icon::Mic, head, ptt_active_ ? "Release when you're done." : mic_result_, 400);
      focus_.count = 2;
      ui::row(canvas_, kBottomCtaY - kRowPitch, Row{"Try again", RowKind::Plain, {}, false, Icon::Update, true},
              focus_.index == 0);
      ui::row(canvas_, kBottomCtaY, Row{"Continue", RowKind::Action, {}, false, Icon::ChevronRight, true},
              focus_.index == 1);
      break;
    }
    case ScreenId::OnboardingDone: {
      if (tips_page_ <= 0) {
        canvas_.stroke_circle(kCanvasW / 2, 250, 56, Gray::G0, 3);
        ui::icon(canvas_, Icon::Check, kCanvasW / 2, 250, 60, Gray::G0);
        centered(canvas_, 330, "You're all set", Canvas::TextRole::ScreenTitle);
        centered(canvas_, 384, cfg_.device_name.empty() ? std::string(kProductName) : cfg_.device_name,
                 Canvas::TextRole::Secondary);
        canvas_.draw_text_wrapped(kSideMargin + 30, 436, kContentW - 60, kLineGap,
                                  "Take a 30-second tour of the controls, or start using Pocket.",
                                  Canvas::TextRole::Secondary, Gray::G0);
        focus_.count = 2;
        ui::button(canvas_, kBottomCtaY - kRowPitch, "Go to Home", focus_.index == 0);
        ui::row(canvas_, kBottomCtaY, Row{"Controls tour", RowKind::Nav}, focus_.index == 1);
      } else {
        static const char* kTitles[3] = {"The dial", "Back button", "Power"};
        static const char* kBodies[3] = {
            "On the left side. Turn to move between items, press the dial to choose.",
            "On the right side, below Power. Press to go back. Hold in Notes and Lists to dictate.",
            "On the right side, above the back button. Press to go Home. Hold to lock \xE2\x80\x94 the screen "
            "stays on with no power at all."};
        const int t = std::min(tips_page_, 3) - 1;
        canvas_.draw_text(kSideMargin, kContentTop, kTitles[t], Canvas::TextRole::ScreenTitle, Gray::G0);
        device_diagram(canvas_, t);
        canvas_.draw_text_wrapped(kSideMargin, 470, kContentW, kLineGap, kBodies[t], Canvas::TextRole::Body, Gray::G0);
        page_dots(canvas_, t, 3, kBottomCtaY - 28);
        ui::button(canvas_, kBottomCtaY, t == 2 ? "Go to Home" : "Next", true);
      }
      break;
    }
    default:
      break;
  }
}

void App::handle_onboarding(InputEvent e) {
  ScreenId s = nav_.current();

  // After setup, onboarding screens are only reachable as tools (Controls tour, Add
  // Wi-Fi, Link, Change PIN). Back must leave them — never walk the setup chain, which
  // would let anyone reach PIN Set without knowing the current PIN.
  if (e == InputEvent::Back && cfg_.onboarding_complete && s != ScreenId::OnboardingPinSet &&
      s != ScreenId::OnboardingPinConfirm && s != ScreenId::OnboardingWifiPassword &&
      s != ScreenId::OnboardingWifiList && s != ScreenId::OnboardingCompanionQr) {
    if (s == ScreenId::OnboardingDone && tips_page_ > 1) {
      --tips_page_;
      mark_content_dirty();
      return;
    }
    tips_page_ = 0;
    if (!nav_.pop()) nav_.reset(ScreenId::Home);
    focus_.index = 0;
    after_nav();
    return;
  }
  // PIN set/confirm: digit-level Back is handled in the PIN branch below.
  if (e == InputEvent::Back && s != ScreenId::OnboardingPinSet && s != ScreenId::OnboardingPinConfirm) {
    switch (s) {
      case ScreenId::OnboardingWelcome:
        break;
      case ScreenId::OnboardingWifiPassword:
        // Add-network SoftAP: Back is ignored in handle() (RF glitch). Onboarding still allows Back.
        wifi_sta_connecting_ = false;
        wifi_add_session_ = false;
        wifi_softap_start_pending_ = false;
        wifi_rf_guard_until_ms_ = 0;
        wifi_.stop_provision();
        if (cfg_.onboarding_complete) {
          // Add-network SoftAP — return to Settings Wi‑Fi, not the setup list.
          wifi_ui_page_ = 0;
          nav_.return_to(ScreenId::SettingsWifi);
        } else {
          nav_.replace(ScreenId::OnboardingSdCard);
        }
        after_nav();
        break;
      case ScreenId::OnboardingWifiList:
        if (cfg_.onboarding_complete) {
          nav_.return_to(ScreenId::SettingsWifi);
        } else {
          nav_.replace(ScreenId::OnboardingSdCard);
        }
        after_nav();
        break;
      case ScreenId::OnboardingSdCard:
        sd_waiting_eject_ = false;
        nav_.replace(ScreenId::OnboardingWelcome);
        after_nav();
        break;
      case ScreenId::OnboardingCompanionDownload:
        nav_.replace(ScreenId::OnboardingWelcome);
        after_nav();
        break;
      case ScreenId::OnboardingCompanionQr:
        if (cfg_.onboarding_complete) {
          nav_.return_to(ScreenId::SettingsCloud);
        } else {
          // Re-enter Link SoftAP phase
          begin_softap_link();
          return;
        }
        after_nav();
        break;
      case ScreenId::OnboardingPinLength:
        nav_.replace(ScreenId::OnboardingCompanionQr);
        after_nav();
        break;
      case ScreenId::OnboardingTimezone:
        nav_.replace(ScreenId::OnboardingPinConfirm);
        after_nav();
        break;
      case ScreenId::OnboardingMicTest:
        nav_.replace(ScreenId::OnboardingTimezone);
        after_nav();
        break;
      case ScreenId::OnboardingDone:
        nav_.replace(ScreenId::OnboardingMicTest);
        after_nav();
        break;
      default:
        break;
    }
    return;
  }

  if (s == ScreenId::OnboardingWelcome && e == InputEvent::Select) {
    if (cfg_.device_name.empty()) cfg_.device_name = "Pocket";
    store_.save(cfg_);
    play_sound(SoundId::Click);
    // Straight on to Storage: the Link step's QR opens Pocket Companion in the browser, so a
    // separate "get the app" step only slowed setup down.
    sd_waiting_eject_ = false;
    if (storage_) {
      storage_->probe();
      sd_kind_ = storage_->classify();
    } else {
      sd_kind_ = SdContentKind::Absent;
    }
    focus_.index = 0;
    nav_.replace(ScreenId::OnboardingSdCard);
    after_nav();
    return;
  }

  if (s == ScreenId::OnboardingCompanionDownload && e == InputEvent::Select) {
    play_sound(SoundId::Click);
    sd_waiting_eject_ = false;
    if (storage_) {
      storage_->probe();
      sd_kind_ = storage_->classify();
    } else {
      sd_kind_ = SdContentKind::Absent;
    }
    focus_.index = 0;
    nav_.replace(ScreenId::OnboardingSdCard);
    after_nav();
    return;
  }

  if (s == ScreenId::OnboardingSdCard) {
    if (sd_waiting_eject_) return;
    if (e == InputEvent::Up) {
      focus_.move(-1);
      mark_content_dirty();
      return;
    }
    if (e == InputEvent::Down) {
      focus_.move(1);
      mark_content_dirty();
      return;
    }
    if (e != InputEvent::Select) return;
    play_sound(SoundId::Click);

    auto refresh_sd = [&]() {
      if (storage_) {
        storage_->probe();
        sd_kind_ = storage_->classify();
      } else {
        sd_kind_ = SdContentKind::Absent;
      }
    };

    if (sd_kind_ == SdContentKind::Absent) {
      if (focus_.index == 0) {
        refresh_sd();
        mark_content_dirty();
      } else {
        begin_softap_link();
      }
      return;
    }
    // Present card: Reformat and continue | Eject (same as anytime SD gate).
    if (focus_.index == 0) {
      if (reformat_sd_with_feedback()) {
        storage_->unmount();
        if (storage_->probe()) storage_->music_ensure_root();
        play_sound(SoundId::Success);
        begin_softap_link();
      } else {
        play_sound(SoundId::Attention);
        mark_content_dirty();
      }
    } else {
      if (storage_) storage_->unmount();
      sd_waiting_eject_ = true;
      last_sd_poll_ms_ = 0;
      play_sound(SoundId::Attention);
      mark_content_dirty();
    }
    return;
  }

  if (s == ScreenId::OnboardingWifiList) {
    constexpr int kMaxVisible = 6;
    const int shown = std::min(static_cast<int>(wifi_networks_.size()), kMaxVisible);
    focus_.count = shown + 1;
    if (e == InputEvent::Up) {
      focus_.move(-1);
      mark_content_dirty();
    } else if (e == InputEvent::Down) {
      focus_.move(1);
      mark_content_dirty();
    } else if (e == InputEvent::Select) {
      if (focus_.index >= shown) {
        wifi_networks_ = wifi_.scan();
        mark_content_dirty();
      } else {
        cfg_.wifi_ssid = wifi_networks_[focus_.index];
        wifi_password_.clear();
        std::string ap;
        std::string pass;
        if (!wifi_.start_provision(cfg_.wifi_ssid, &ap, &pass)) {
          error_msg_ = "Couldn't start phone setup.";
          error_until_ms_ = now_ms_ + 3000;
          mark_content_dirty();
          return;
        }
        wifi_ap_ssid_ = ap.empty() ? wifi_.provision_ap_ssid() : ap;
        wifi_ap_pass_ = pass.empty() ? wifi_.provision_ap_password() : pass;
        last_wifi_prov_poll_ms_ = 0;
        focus_.index = 0;
        nav_.replace(ScreenId::OnboardingWifiPassword);
        after_nav();
      }
    }
    return;
  }

  if (s == ScreenId::OnboardingWifiPassword) {
    const bool already_online = wifi_.connected();
    focus_.count = already_online ? 2 : 1;
    if (e == InputEvent::Up) {
      focus_.move(-1);
      mark_content_dirty();
    } else if (e == InputEvent::Down) {
      focus_.move(1);
      mark_content_dirty();
    } else if (e == InputEvent::Select) {
      const int cancel_i = already_online ? 1 : 0;
      const int skip_i = already_online ? 0 : -1;
      if (focus_.index == skip_i) {
        play_sound(SoundId::Click);
        wifi_add_session_ = false;
        wifi_softap_start_pending_ = false;
        wifi_rf_guard_until_ms_ = 0;
        wifi_.stop_provision();
        if (cfg_.onboarding_complete) {
          wifi_ui_page_ = 0;
          nav_.return_to(ScreenId::SettingsWifi);
          after_nav();
        } else {
          play_sound(mint_pair_session() ? SoundId::Success : SoundId::Attention);
          focus_.index = 0;
          nav_.replace(ScreenId::OnboardingCompanionQr);
          after_nav();
        }
      } else if (focus_.index == cancel_i) {
        play_sound(SoundId::Click);
        wifi_sta_connecting_ = false;
        wifi_add_session_ = false;
        wifi_softap_start_pending_ = false;
        wifi_rf_guard_until_ms_ = 0;
        wifi_.stop_provision();
        if (cfg_.onboarding_complete) {
          wifi_ui_page_ = 0;
          nav_.return_to(ScreenId::SettingsWifi);
        } else {
          nav_.replace(ScreenId::OnboardingSdCard);
        }
        after_nav();
      }
      // Waiting… — credentials arrive via App::tick SoftAP poll
    }
    return;
  }

  if (s == ScreenId::OnboardingCompanionQr) {
    if (pair_status_ == "claimed") {
      focus_.index = 0;
      if (cfg_.onboarding_complete) {
        nav_.return_to(ScreenId::SettingsCloud);
      } else {
        nav_.replace(ScreenId::OnboardingPinLength);
      }
      after_nav();
      return;
    }
    focus_.count = 2;  // Waiting… · Refresh code — no Skip
    if (pair_code_.empty()) {
      // Error state: Try again · Choose a different Wi-Fi.
      if (e == InputEvent::Up || e == InputEvent::Down) {
        focus_.move(e == InputEvent::Down ? 1 : -1);
        mark_content_dirty();
      } else if (e == InputEvent::Select && focus_.index == 0) {
        play_sound(mint_pair_session() ? SoundId::Success : SoundId::Attention);
        focus_.index = 0;
        after_nav();
      } else if (e == InputEvent::Select && focus_.index == 1) {
        play_sound(SoundId::Click);
        wifi_networks_ = wifi_.scan();
        focus_.index = 0;
        nav_.replace(ScreenId::OnboardingWifiList);
        after_nav();
      }
      return;
    }
    if (e == InputEvent::Up) {
      focus_.move(-1);
      mark_content_dirty();
    } else if (e == InputEvent::Down) {
      focus_.move(1);
      mark_content_dirty();
    } else if (e == InputEvent::Select) {
      if (focus_.index == 1 || pair_status_ == "expired") {
        if (mint_pair_session()) {
          play_sound(SoundId::Click);
          mark_content_dirty();
        } else {
          play_sound(SoundId::Attention);
          mark_content_dirty();
        }
      }
      // index 0 = waiting — link arrives via tick poll
    }
    return;
  }

  if (s == ScreenId::OnboardingPinLength) {
    focus_.count = 2;
    if (e == InputEvent::Up) {
      focus_.move(-1);
      mark_content_dirty();
    } else if (e == InputEvent::Down) {
      focus_.move(1);
      mark_content_dirty();
    } else if (e == InputEvent::Select) {
      cfg_.pin_length = focus_.index == 0 ? 4 : 6;
      pin_entry_.clear();
      pin_spin_count_ = 0;
      pin_ignore_select_until_ms_ = 0;
      pin_enter_empty_slot();
      nav_.replace(ScreenId::OnboardingPinSet);
      after_nav();
    }
    return;
  }

  if (s == ScreenId::OnboardingPinSet || s == ScreenId::OnboardingPinConfirm) {
    if (e == InputEvent::Up) {
      pin_ignore_select_until_ms_ = 0;
      apply_pin_digit_delta(-1);
    } else if (e == InputEvent::Down) {
      pin_ignore_select_until_ms_ = 0;
      apply_pin_digit_delta(1);
    } else if (e == InputEvent::Back) {
      if (pin_entry_.empty()) {
        if (s == ScreenId::OnboardingPinConfirm) {
          nav_.replace(ScreenId::OnboardingPinSet);
          pin_entry_.clear();
          pin_ignore_select_until_ms_ = 0;
          pin_enter_empty_slot();
          after_nav();
        } else if (cfg_.onboarding_complete) {
          // Change PIN cancelled — the old PIN stays.
          pin_change_ = false;
          pin_pending_.clear();
          if (!nav_.pop()) nav_.reset(ScreenId::Home);
          after_nav();
        } else {
          nav_.replace(ScreenId::OnboardingPinLength);
          after_nav();
        }
      } else {
        pin_backspace();
        mark_pin_slots_dirty(static_cast<int>(pin_entry_.size()) + 1, static_cast<int>(pin_entry_.size()),
                             true);
      }
    } else if (e == InputEvent::Select) {
      const int prev_i = static_cast<int>(pin_entry_.size());
      if (static_cast<int>(pin_entry_.size()) < cfg_.pin_length) {
        if (!pin_commit_working_digit()) return;
      }
      if (static_cast<int>(pin_entry_.size()) >= cfg_.pin_length) {
        if (s == ScreenId::OnboardingPinSet) {
          pin_pending_ = pin_entry_;
          pin_entry_.clear();
          pin_ignore_select_until_ms_ = 0;
          pin_enter_empty_slot();
          nav_.replace(ScreenId::OnboardingPinConfirm);
          after_nav();
        } else {
          if (pin_entry_ == pin_pending_) {
            set_pin(cfg_, pin_entry_);
            store_.save(cfg_);
            pin_entry_.clear();
            pin_pending_.clear();
            pin_ignore_select_until_ms_ = 0;
            pin_enter_empty_slot();
            if (cfg_.onboarding_complete) {
              pin_change_ = false;
              play_sound(SoundId::Success);
              if (!nav_.pop()) nav_.reset(ScreenId::Home);
              after_nav();
            } else {
              focus_.index = onboarding_tz_index_;
              nav_.replace(ScreenId::OnboardingTimezone);
              after_nav();
            }
          } else {
            error_msg_ = "Those didn't match. Try again.";
            error_until_ms_ = now_ms_ + 2500;
            pin_entry_.clear();
            pin_ignore_select_until_ms_ = 0;
            pin_enter_empty_slot();
            nav_.replace(ScreenId::OnboardingPinSet);
            after_nav();
          }
        }
      } else {
        mark_pin_slots_dirty(prev_i, static_cast<int>(pin_entry_.size()), true);
      }
    }
    return;
  }

  if (s == ScreenId::OnboardingTimezone) {
    focus_.count = kTzCount + 3;
    if (e == InputEvent::Up) {
      focus_.move(-1);
      mark_content_dirty();
    } else if (e == InputEvent::Down) {
      focus_.move(1);
      mark_content_dirty();
    } else if (e == InputEvent::Select) {
      if (focus_.index < kTzCount) {
        onboarding_tz_index_ = focus_.index;
        cfg_.tz_id = kTimezones[focus_.index];
        mark_content_dirty();
      } else if (focus_.index == kTzCount) {
        cfg_.time_format = 12;
        mark_content_dirty();
      } else if (focus_.index == kTzCount + 1) {
        cfg_.time_format = 24;
        mark_content_dirty();
      } else {
        store_.save(cfg_);
        focus_.index = 1;
        mic_result_.clear();
        nav_.replace(ScreenId::OnboardingMicTest);
        after_nav();
      }
    }
    return;
  }

  if (s == ScreenId::OnboardingMicTest) {
    if (e == InputEvent::PttStart) {
      ptt_active_ = true;
      mic_capturing_ = true;
      if (audio_) audio_->start_capture();
      mark_content_dirty();
    } else if (e == InputEvent::PttStop) {
      ptt_active_ = false;
      mic_capturing_ = false;
      const MicCaptureResult r = audio_ ? audio_->stop_capture() : MicCaptureResult{};
      mic_last_level_ = r.ok ? r.level_percent : -1;
      mic_last_ok_ = r.ok && r.level_percent >= 6;
      mic_result_ = !r.ok ? "Mic needs real hardware to test."
                          : (mic_last_ok_ ? "Mic OK — picked up sound" : "No sound detected — try again");
      mark_content_dirty();
    } else {
      focus_.count = 2;
      if (e == InputEvent::Up) {
        focus_.move(-1);
        mark_content_dirty();
      } else if (e == InputEvent::Down) {
        focus_.move(1);
        mark_content_dirty();
      } else if (e == InputEvent::Select) {
        if (focus_.index == 0) {
          mic_result_.clear();
          mark_content_dirty();
        } else {
          cfg_.stt_path = 0;
          store_.save(cfg_);
          tips_page_ = 0;
          nav_.replace(ScreenId::OnboardingDone);
          after_nav();
        }
      }
    }
    return;
  }

  if (s == ScreenId::OnboardingDone) {
    if (tips_page_ <= 0) {
      focus_.count = 2;
      if (e == InputEvent::Up) {
        focus_.move(-1);
        mark_content_dirty();
        return;
      }
      if (e == InputEvent::Down) {
        focus_.move(1);
        mark_content_dirty();
        return;
      }
      if (e == InputEvent::Select) {
        if (focus_.index == 1) {
          tips_page_ = 1;
          mark_content_dirty();
          return;
        }
        // Go to Home (index 0)
        if (cfg_.onboarding_complete) {
          cfg_.seen_whats_new_build = cfg_.fw_build_id;
          store_.save(cfg_);
          nav_.pop();
          after_nav();
          return;
        }
        play_sound(SoundId::Success);
        cfg_.onboarding_complete = true;
        if (cfg_.device_name.empty()) cfg_.device_name = "Pocket";
        cfg_.seen_whats_new_build = cfg_.fw_build_id;
        store_.save(cfg_);
        go_home();
        return;
      }
      return;
    }
    if (e == InputEvent::Back && tips_page_ > 0) {
      --tips_page_;
      if (tips_page_ == 0) focus_.index = 0;
      mark_content_dirty();
      return;
    }
    if (e == InputEvent::Select) {
      if (tips_page_ < 3) {
        ++tips_page_;
        mark_content_dirty();
        return;
      }
      if (cfg_.onboarding_complete) {
        cfg_.seen_whats_new_build = cfg_.fw_build_id;
        store_.save(cfg_);
        nav_.pop();
        after_nav();
        return;
      }
      play_sound(SoundId::Success);
      cfg_.onboarding_complete = true;
      if (cfg_.device_name.empty()) cfg_.device_name = "Pocket";
      cfg_.seen_whats_new_build = cfg_.fw_build_id;
      store_.save(cfg_);
      go_home();
    }
  }
}

}  // namespace pocket
