#include "pocket/app.hpp"
#include "pocket/ui.hpp"
#include "pocket/json_lite.hpp"
#include "pocket/release.hpp"
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace pocket {

namespace {
/** Bottom band repainted for the status pill (toast: up to 2 lines + margins). */
constexpr int kStatusPillBand = 140;
}  // namespace


extern void draw_status_bar_impl(Canvas& c, const DeviceConfig& cfg, int hour, int minute, bool wifi_ok,
                                 int battery_pct, bool time_ok, bool time_needs_sync = false);

static const char* kWeekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static const char* kTimezones[] = {"America/New_York", "America/Chicago", "America/Denver",
                                   "America/Los_Angeles", "America/Phoenix", "UTC", "Europe/London"};

App::App(ConfigStore& store, PlatformClock& clock, PlatformWifi& wifi, PlatformCloud& cloud,
         PlatformDisplay& display, PlatformStorage* storage, PlatformAudio* audio,
         PlatformIdentity* identity, PlatformOta* ota)
    : store_(store),
      clock_(clock),
      wifi_(wifi),
      cloud_(cloud),
      display_(display),
      storage_(storage),
      audio_(audio),
      identity_(identity),
      ota_(ota) {}

void App::set_build_id(std::string_view id) {
  pending_build_id_.assign(id.begin(), id.end());
}

void App::boot() {
  cfg_ = store_.load();
  cloud_.set_device_token(cfg_.device_token);
  if (!cfg_.fw_build_id.empty()) {
    // Keep flashed identity over any stale NVS version string.
  } else if (!cfg_.fw_version.empty() && cfg_.fw_version.rfind("POCKET-LIVE", 0) == 0) {
    cfg_.fw_build_id = cfg_.fw_version;
  }
  // Prefer identity stamped by app_main (kBuildId) when set before boot.
  if (!pending_build_id_.empty()) {
    // A different build than last boot = an update just installed → welcome screen.
    if (cfg_.onboarding_complete && !cfg_.fw_build_id.empty() && cfg_.fw_build_id != pending_build_id_) {
      show_update_welcome_ = true;
    }
    if (cfg_.fw_build_id != pending_build_id_) {
      cfg_.fw_build_id = pending_build_id_;
      store_.save(cfg_);
    }
  }
  // Consumer-facing version is always the product semver — never a flash marker.
  cfg_.fw_version = kConsumerVersion;
  if (cfg_.device_name.empty()) cfg_.device_name = kProductName;
  if (cfg_.device_id.empty()) {
    if (identity_) cfg_.device_id = identity_->device_uuid();
    if (cfg_.device_id.empty()) cfg_.device_id = "00000000-0000-4000-8000-000000000001";
    store_.save(cfg_);
  }
  now_ms_ = clock_.now_ms();
  last_input_ms_ = now_ms_;
  if (audio_) audio_->set_volume(cfg_.volume_percent);
  display_.set_brightness(cfg_.brightness_percent);
  refresh_.set_ghosting_budget(cfg_.ghosting_frequent ? RefreshPolicy::kGhostingNFrequent
                                                       : RefreshPolicy::kGhostingN);
  load_local_notes();
  load_local_lists();
  load_local_passes();
  load_alarms();
  load_weather_cache();
  music_load_local();
  reading_load_local();
  calendar_load_cache();
  if (storage_ && storage_->data_ensure_root()) {
    std::string st;
    JsonValue v;
    if (read_file_recover(storage_->data_root() + "/backup_state.json", st) && json_parse(st, v) && v.is_object())
      last_backup_unix_ = v.get_int("last");
  }
  apply_cloud_offline_grace();
  if (!cfg_.onboarding_complete) {
    const ScreenId resume = resume_onboarding_screen();
    if (resume == ScreenId::OnboardingWifiPassword || resume == ScreenId::OnboardingCompanionQr) {
      // Pocket already knows a network (it joined before this restart): rejoin it and go
      // straight on to Link. The phone setup network only comes back if rejoining fails.
      if (!cfg_.wifi_known.empty()) {
        setup_rejoin_pending_ = true;
        nav_.reset(ScreenId::OnboardingWifiConnecting);
        after_nav();
        return;
      }
      // SoftAP / pair need live sessions — restart Link without Welcome → Download → SD.
      begin_softap_link();
      return;
    }
    nav_.reset(resume);
  } else {
    nav_.reset(ScreenId::Lock);
    // Card already seated at boot is not a hot-insert — only interrupt on new insert.
    if (storage_) {
      storage_->probe();
      sd_was_present_ = storage_->present();
    }
    // Reconnect to a known network after power-cycle (password now persisted).
    wifi_boot_reconnect_done_ = false;
    last_wifi_reconnect_ms_ = 0;
  }
  after_nav();
}

static bool pin_hash_set(const DeviceConfig& cfg) {
  for (uint8_t b : cfg.pin_hash) {
    if (b != 0) return true;
  }
  return false;
}

ScreenId App::resume_onboarding_screen() const {
  // Prefer farthest durable progress so reboot does not restart Welcome.
  if (pin_hash_set(cfg_)) {
    return ScreenId::OnboardingTimezone;
  }
  if (cfg_.companion_linked) {
    return ScreenId::OnboardingPinLength;
  }
  if (!cfg_.wifi_ssid.empty() || !cfg_.wifi_known.empty()) {
    // SoftAP / Link again so phone can confirm or send another network.
    return ScreenId::OnboardingWifiPassword;
  }
  return ScreenId::OnboardingWelcome;
}

void App::ensure_softap_credentials_shown() {
  if (!wifi_.provisioning()) {
    std::string ap;
    std::string pass;
    if (wifi_.start_provision(cfg_.wifi_ssid, &ap, &pass)) {
      wifi_ap_ssid_ = ap.empty() ? wifi_.provision_ap_ssid() : ap;
      wifi_ap_pass_ = pass.empty() ? wifi_.provision_ap_password() : pass;
    }
  } else {
    wifi_ap_ssid_ = wifi_.provision_ap_ssid();
    wifi_ap_pass_ = wifi_.provision_ap_password();
  }
}

bool App::quiet_now() const {
  if (!cfg_.quiet_enabled || !clock_.time_valid()) return false;
  int h = 0, m = 0, wd = 0, mo = 0, d = 0;
  clock_.local_hm(h, m, wd, mo, d);
  const int now = h * 60 + m, a = cfg_.quiet_start_min, b = cfg_.quiet_end_min;
  if (a == b) return false;
  return a < b ? (now >= a && now < b) : (now >= a || now < b);  // window may cross midnight
}

void App::play_sound(SoundId id) {
  if (quiet_now()) return;
  if (audio_) audio_->play(id);
}

void App::play_sound_urgent(SoundId id) {
  if (audio_) audio_->play(id);
}

void App::ring_find_my_pocket() {
  // Companion asked "where is it?" — chime loudly a few times and say so if someone looks.
  if (audio_) audio_->set_volume(100);
  for (int i = 0; i < 4; ++i) {
    play_sound_urgent(SoundId::Attention);
    clock_.sleep_ms(700);
  }
  if (audio_) audio_->set_volume(cfg_.volume_percent);
  if (nav_.current() != ScreenId::Lock && nav_.current() != ScreenId::Pin) {
    error_msg_ = "Found it \xE2\x80\x94 Pocket Companion asked Pocket to chime.";
    error_until_ms_ = now_ms_ + 5000;
    transient_error_armed_ = true;
    mark_content_dirty();
  }
}

void App::mark_content_dirty() {
  // Only refuse to downgrade when a full refresh is already pending.
  if (dirty_ && dirty_kind_ == DirtyKind::FullCanvas) return;
  dirty_ = true;
  dirty_kind_ = DirtyKind::ContentBand;
}

void App::mark_status_dirty() {
  if (dirty_ && (dirty_kind_ == DirtyKind::FullCanvas || dirty_kind_ == DirtyKind::ContentBand ||
                 dirty_kind_ == DirtyKind::Region))
    return;
  dirty_ = true;
  dirty_kind_ = DirtyKind::StatusBar;
}

void App::mark_region_dirty(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  // Don't shrink an already-pending full or content-band update.
  if (dirty_ && (dirty_kind_ == DirtyKind::FullCanvas || dirty_kind_ == DirtyKind::ContentBand)) return;
  if (dirty_ && dirty_kind_ == DirtyKind::Region) {
    const int x1 = std::min(dirty_rx_, x);
    const int y1 = std::min(dirty_ry_, y);
    const int x2 = std::max(dirty_rx_ + dirty_rw_, x + w);
    const int y2 = std::max(dirty_ry_ + dirty_rh_, y + h);
    dirty_rx_ = x1;
    dirty_ry_ = y1;
    dirty_rw_ = x2 - x1;
    dirty_rh_ = y2 - y1;
    return;
  }
  dirty_ = true;
  dirty_kind_ = DirtyKind::Region;
  dirty_rx_ = x;
  dirty_ry_ = y;
  dirty_rw_ = w;
  dirty_rh_ = h;
}

void App::pin_band_geometry(int& x, int& y, int& w, int& h) const {
  // Progress + slots + hint + error/lockout lines (must cover "Incorrect PIN").
  x = 0;
  y = pin_draw_band_top_ - 44;
  w = kCanvasW;
  h = std::min(kCanvasH - y, 460 - y);
}

void App::mark_pin_slots_dirty(int slot_a, int slot_b, bool with_chrome) {
  constexpr int kSlotW = 52;
  constexpr int kSlotH = 72;
  constexpr int kGap = 14;
  const int n = std::max(1, static_cast<int>(cfg_.pin_length));
  const int total = n * kSlotW + (n - 1) * kGap;
  const int x0 = (kCanvasW - total) / 2;
  const int slot_y = pin_draw_band_top_;
  const int i0 = std::clamp(std::min(slot_a, slot_b), 0, n - 1);
  const int i1 = std::clamp(std::max(slot_a, slot_b), 0, n - 1);
  const int pad = 8;
  if (with_chrome) {
    // Include error / lockout copy under the slots (y≈420–448).
    const int top = slot_y - 44;
    mark_region_dirty(0, top, kCanvasW, std::min(kCanvasH - top, 460 - top));
    return;
  }
  mark_region_dirty(x0 + i0 * (kSlotW + kGap) - pad, slot_y - pad,
                    (i1 - i0) * (kSlotW + kGap) + kSlotW + 2 * pad, kSlotH + 2 * pad);
}

void App::mark_pin_dirty() {
  ++pin_spin_count_;
  // Always chrome-band: tiny single-slot partials ghost the previous digit into the
  // wrong slot under 0xFF, and can leave lock-motif ink between slots.
  mark_pin_slots_dirty(static_cast<int>(pin_entry_.size()), static_cast<int>(pin_entry_.size()),
                       /*with_chrome=*/true);
}

bool App::is_pin_entry_screen() const {
  const ScreenId s = nav_.current();
  return s == ScreenId::Pin || s == ScreenId::OnboardingPinSet || s == ScreenId::OnboardingPinConfirm;
}

void App::apply_coalesced_rotary(int delta) {
  if (delta == 0) return;
  // Turning the dial is using Pocket — Home/PIN spins skip handle(), so stamp it here
  // (they used to let idle-lock fire mid-browse).
  now_ms_ = clock_.now_ms();
  last_input_ms_ = now_ms_;
  if (is_pin_entry_screen()) {
    apply_pin_digit_delta(delta);
    return;
  }
  if (is_home_screen()) {
    apply_home_focus_delta(delta);
    return;
  }
  // In-page lists/settings/clock: replay steps without intermediate presents so a
  // bounce/detent burst updates once (one press → one correct region).
  defer_handle_present_ = true;
  const InputEvent step = delta > 0 ? InputEvent::Down : InputEvent::Up;
  const int n = delta > 0 ? delta : -delta;
  for (int i = 0; i < n; ++i) handle(step);
  defer_handle_present_ = false;
}

void App::apply_pin_digit_delta(int delta) {
  if (!is_pin_entry_screen()) return;
  if (now_ms_ < pin_lockout_until_ms_) return;
  if (delta == 0) return;
  // A real spin after a commit means the next Select is intentional — drop bounce guard.
  pin_ignore_select_until_ms_ = 0;
  // Normalize into 0..9 via positive modulo.
  int v = (pin_digit_working_ - '0') + delta;
  v %= 10;
  if (v < 0) v += 10;
  pin_digit_working_ = static_cast<char>('0' + v);
  pin_spun_since_focus_ = true;
  focus_.index = static_cast<int>(pin_entry_.size());
  mark_pin_dirty();
}

char App::pin_slot_value(int i) const {
  if (i < 0 || i >= cfg_.pin_length) return '?';
  if (i < static_cast<int>(pin_entry_.size())) return pin_entry_[static_cast<size_t>(i)];
  if (i == static_cast<int>(pin_entry_.size())) return pin_digit_working_;
  return '0';
}

void App::pin_enter_empty_slot() {
  // New / cleared slot must show 0 — never a stale spin from the previous index.
  pin_digit_working_ = '0';
  pin_spun_since_focus_ = false;
  focus_.index = static_cast<int>(pin_entry_.size());
}

bool App::pin_commit_working_digit() {
  if (static_cast<int>(pin_entry_.size()) >= cfg_.pin_length) return false;
  // Time-windowed bounce guard: ignore Select only while within the arm window.
  if (pin_ignore_select_until_ms_ != 0 && now_ms_ < pin_ignore_select_until_ms_) {
    return false;
  }
  pin_ignore_select_until_ms_ = 0;
  const bool spun = pin_spun_since_focus_;
  pin_entry_.push_back(pin_digit_working_);
  pin_enter_empty_slot();
  // Arm briefly after a spun commit so mechanical bounce cannot skip a slot with '0'.
  // Unspun commits (e.g. 0000 via repeated Select) stay unarmed.
  if (spun) {
    constexpr uint32_t kSelectBounceMs = 120;
    pin_ignore_select_until_ms_ = now_ms_ + kSelectBounceMs;
  }
  return true;
}

void App::pin_backspace() {
  if (pin_entry_.empty()) return;
  pin_entry_.pop_back();
  pin_ignore_select_until_ms_ = 0;
  pin_enter_empty_slot();
}

bool App::try_present(bool full) {
  // Never clear dirty / call into the panel while a LUT is mid-flight — present()
  // would no-op on device and a blind dirty_=false bricks Lock→PIN paint (v91).
  if (display_.busy()) return false;
  present_canvas(full);
  return true;
}

bool App::keeps_awake() const {
  // Things you're doing even when not pressing anything.
  if (ptt_active_ || mic_capturing_ || ota_installing_ || sd_formatting_) return true;
  if (music_playing_) return true;
  const ScreenId s = nav_.current();
  if (s == ScreenId::ClockTimerRun && timer_running_) return true;
  if (s == ScreenId::SettingsUpdateProgress || s == ScreenId::SettingsUpdateAvailable) return true;
  return false;
}

uint32_t App::idle_lock_after_ms() const {
  uint32_t ms = static_cast<uint32_t>(cfg_.idle_lock_s) * 1000u;
  // Reading a page or a note takes longer than the auto-lock delay — give it 10 minutes.
  const ScreenId s = nav_.current();
  if (s == ScreenId::ReadingBook || s == ScreenId::NotesDetail || s == ScreenId::WeatherMain ||
      s == ScreenId::PassDetail || s == ScreenId::ClockFace)
    ms = std::max<uint32_t>(ms, 10u * 60u * 1000u);
  return ms;
}

void App::maybe_idle_clean() {
  // Kindle-style: once partials have left residue and the user pauses, one clean full
  // refresh wipes the ghosting instead of letting it pile up until a forced flash mid-use.
  const int need = cfg_.ghosting_frequent ? RefreshPolicy::kIdleCleanDebtFrequent : RefreshPolicy::kIdleCleanDebt;
  if (refresh_.clean_debt() < need) return;
  if (now_ms_ < last_input_ms_ || now_ms_ - last_input_ms_ < kIdleCleanAfterMs) return;
  if (now_ms_ < last_present_ms_ || now_ms_ - last_present_ms_ < 1200) return;
  const ScreenId s = nav_.current();
  if (s == ScreenId::Pin || s == ScreenId::OnboardingPinSet || s == ScreenId::OnboardingPinConfirm ||
      s == ScreenId::ClockRinging || s == ScreenId::SettingsUpdateProgress || ptt_active_)
    return;
  if (display_.busy()) return;
  present_canvas(true);
}

void App::show_status(std::string_view msg) {
  const ScreenId s = nav_.current();
  if (s == ScreenId::Lock || s == ScreenId::Pin || s == ScreenId::ClockRinging ||
      s == ScreenId::SettingsUpdateProgress)
    return;
  status_msg_.assign(msg.data(), msg.size());
  display_.wait_idle(2500);
  mark_region_dirty(0, kCanvasH - kStatusPillBand, kCanvasW, kStatusPillBand);
  if (try_present(false)) dirty_ = false;
}

void App::flush_dirty() {
  if (!status_msg_.empty()) {
    status_msg_.clear();
    now_ms_ = clock_.now_ms();
    last_input_ms_ = now_ms_;  // the user was waiting on Pocket, not away from it
    mark_region_dirty(0, kCanvasH - kStatusPillBand, kCanvasW, kStatusPillBand);
  }
  if (!dirty_) {
    maybe_idle_clean();
    return;
  }
  // Never block the input path on a mid-flight e-ink refresh — keep dirty and
  // apply further rotary ticks to logical state; present once the panel is idle.
  if (!try_present(want_full_)) return;
  dirty_ = false;
  want_full_ = false;
}

void App::begin_softap_link() {
  if (wifi_.connected()) {
    // Already on home Wi‑Fi — straight to Link (its error state offers Try again /
    // Choose a different Wi-Fi if Pocket Cloud can't be reached).
    play_sound(mint_pair_session() ? SoundId::Success : SoundId::Attention);
    focus_.index = 0;
    wifi_add_session_ = false;
    nav_.replace(ScreenId::OnboardingCompanionQr);
    after_nav();
    return;
  }
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
  wifi_sta_connecting_ = false;
  last_wifi_prov_poll_ms_ = 0;
  focus_.index = 0;
  wifi_add_session_ = true;
  now_ms_ = clock_.now_ms();
  last_input_ms_ = now_ms_;
  // Stay on SoftAP screen if already there — avoid full remount bounce.
  if (nav_.current() == ScreenId::OnboardingWifiPassword) {
    mark_content_dirty();
    return;
  }
  nav_.replace(ScreenId::OnboardingWifiPassword);
  after_nav();
  now_ms_ = clock_.now_ms();
  last_input_ms_ = now_ms_;
}

bool App::connect_and_remember(const std::string& ssid, const std::string& password) {
  if (ssid.empty()) return false;
  wifi_password_ = password;
  show_status("Connecting to " + ssid + "\xE2\x80\xA6");
  const bool ok = wifi_.connect(ssid, password);
  if (ok) {
    wifi_known_upsert(cfg_, ssid, password);
    store_.save(cfg_);
    wifi_boot_reconnect_done_ = true;
    last_wifi_reconnect_ms_ = now_ms_;
  }
  return ok;
}

void App::arm_wifi_rf_guard() {
  now_ms_ = clock_.now_ms();
  last_input_ms_ = now_ms_;
  // SoftAP APSTA TX couples into active-low PWR/BOOT — suppress Lock gestures ~3s.
  wifi_rf_guard_until_ms_ = now_ms_ + 3000u;
  input_resync_pending_ = true;
}

bool App::wifi_rf_guard_active() const {
  return wifi_rf_guard_until_ms_ != 0 && now_ms_ < wifi_rf_guard_until_ms_;
}

void App::maybe_setup_rejoin() {
  if (!setup_rejoin_pending_ || nav_.current() != ScreenId::OnboardingWifiConnecting) return;
  // Let "Connecting…" paint and the panel settle before the radio works (power peaks).
  if (dirty_ || display_.busy()) return;
  setup_rejoin_pending_ = false;
  std::vector<WifiKnownNetwork> order = cfg_.wifi_known;
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i].ssid == cfg_.wifi_ssid && i != 0) {
      std::swap(order[0], order[i]);
      break;
    }
  }
  bool ok = wifi_.connected();
  for (size_t i = 0; !ok && i < order.size() && i < 3; ++i) {
    wifi_sta_connecting_ = true;
    ok = connect_and_remember(order[i].ssid, order[i].password);
    wifi_sta_connecting_ = false;
    if (ok) cfg_.wifi_ssid = order[i].ssid;
  }
  if (ok) {
    play_sound(mint_pair_session() ? SoundId::Success : SoundId::Attention);
    focus_.index = 0;
    nav_.replace(ScreenId::OnboardingCompanionQr);
    after_nav();
    return;
  }
  error_msg_ = "Couldn't rejoin " + (cfg_.wifi_ssid.empty() ? std::string("your Wi-Fi") : cfg_.wifi_ssid) +
               ". Send it again from your phone.";
  error_until_ms_ = now_ms_ + 6000;
  begin_softap_link();
}

void App::maybe_start_pending_softap_radio() {
  if (!wifi_softap_start_pending_) return;
  // Wait out SoftAP enter full-refresh before RF — concurrent e-ink + APSTA was the
  // enter-path Lock (brownout / PWR glitch while go_lock was already a session no-op).
  if (display_.busy()) return;
  wifi_softap_start_pending_ = false;
  std::string ap;
  std::string pass;
  const std::string preferred =
      wifi_softap_pending_preferred_.empty() ? cfg_.wifi_ssid : wifi_softap_pending_preferred_;
  wifi_softap_pending_preferred_.clear();
  if (!wifi_.start_provision(preferred, &ap, &pass)) {
    // Keep wifi_add_session_ — failed start still flipped radio briefly.
    error_msg_ = "Couldn't start phone setup.";
    error_until_ms_ = now_ms_ + 3000;
    mark_content_dirty();
    arm_wifi_rf_guard();
    return;
  }
  wifi_ap_ssid_ = ap.empty() ? wifi_.provision_ap_ssid() : ap;
  wifi_ap_pass_ = pass.empty() ? wifi_.provision_ap_password() : pass;
  last_wifi_prov_poll_ms_ = 0;
  mark_content_dirty();
  arm_wifi_rf_guard();
}

void App::begin_add_wifi_network() {
  // SoftAP while optionally staying on current STA (APSTA) so user can add another network.
  // v100 enter path: arm session → paint SoftAP (quiet RF) → start APSTA once panel idle.
  wifi_add_session_ = true;
  wifi_sta_connecting_ = false;
  last_wifi_prov_poll_ms_ = 0;
  focus_.index = 0;
  now_ms_ = clock_.now_ms();
  last_input_ms_ = now_ms_;
  arm_wifi_rf_guard();

  std::string ap;
  std::string pass;
  if (wifi_.peek_provision_identity(&ap, &pass)) {
    wifi_ap_ssid_ = ap;
    wifi_ap_pass_ = pass;
  } else {
    wifi_ap_ssid_.clear();
    wifi_ap_pass_.clear();
  }

  if (nav_.current() != ScreenId::OnboardingWifiPassword) {
    nav_.replace(ScreenId::OnboardingWifiPassword);
    after_nav();
  } else {
    mark_content_dirty();
  }

  wifi_softap_pending_preferred_ = cfg_.wifi_ssid;
  wifi_softap_start_pending_ = true;
  if (!display_.busy()) {
    // Host / already-idle panel: start radio immediately after SoftAP paint kick.
    maybe_start_pending_softap_radio();
  }
  now_ms_ = clock_.now_ms();
  last_input_ms_ = now_ms_;
}

void App::maybe_wifi_auto_reconnect() {
  if (!cfg_.onboarding_complete) return;
  if (wifi_.connected() || wifi_.provisioning() || wifi_sta_connecting_) return;
  if (cfg_.wifi_known.empty()) return;
  // SoftAP / pair screens own Wi‑Fi.
  const ScreenId s = nav_.current();
  if (s == ScreenId::OnboardingWifiPassword || s == ScreenId::OnboardingWifiList ||
      s == ScreenId::OnboardingWifiConnecting) {
    return;
  }

  // Away from known networks, scanning every 45 s froze the UI and drained the battery;
  // on the lock face retry every 10 min (unlocking or a Wi-Fi tap retries immediately).
  const uint32_t interval_ms =
      !wifi_boot_reconnect_done_ ? 800u : (s == ScreenId::Lock ? 600000u : 45000u);
  if (last_wifi_reconnect_ms_ != 0 && now_ms_ - last_wifi_reconnect_ms_ < interval_ms) return;
  last_wifi_reconnect_ms_ = now_ms_;
  wifi_sta_connecting_ = true;

  // Prefer last SSID, then other known entries. Try scan match first when possible.
  std::vector<WifiKnownNetwork> order = cfg_.wifi_known;
  if (!cfg_.wifi_ssid.empty()) {
    for (size_t i = 0; i < order.size(); ++i) {
      if (order[i].ssid == cfg_.wifi_ssid) {
        if (i != 0) std::swap(order[0], order[i]);
        break;
      }
    }
  }

  std::vector<std::string> visible;
  // Boot: try preferred blindly first (faster); later retries may scan.
  if (wifi_boot_reconnect_done_) {
    visible = wifi_.scan();
  }

  bool ok = false;
  for (const auto& net : order) {
    if (!visible.empty()) {
      bool seen = false;
      for (const auto& v : visible) {
        if (v == net.ssid) {
          seen = true;
          break;
        }
      }
      if (!seen) continue;
    }
    if (wifi_.connect(net.ssid, net.password)) {
      cfg_.wifi_ssid = net.ssid;
      wifi_password_ = net.password;
      store_.save(cfg_);
      ok = true;
      break;
    }
  }
  // If scan filtered everything out, try preferred blindly once.
  if (!ok && !order.empty() && !visible.empty()) {
    const auto& net = order.front();
    if (wifi_.connect(net.ssid, net.password)) {
      cfg_.wifi_ssid = net.ssid;
      wifi_password_ = net.password;
      store_.save(cfg_);
      ok = true;
    }
  }

  wifi_sta_connecting_ = false;
  wifi_boot_reconnect_done_ = true;
  if (ok) mark_status_dirty();
}

bool App::mint_pair_session() {
  // Right after joining Wi-Fi the link can need a few seconds (DHCP/DNS settle, SoftAP
  // teardown). Retry with backoff for ~10 s and say what's happening meanwhile.
  show_status("Connecting to Pocket Cloud\xE2\x80\xA6");
  display_.wait_idle(1500);  // don't overlap the panel refresh with TLS (power peaks)
  pair_error_.clear();
  static constexpr uint32_t kBackoffMs[] = {0, 1000, 2000, 3000, 4000};
  for (uint32_t wait : kBackoffMs) {
    if (wait) clock_.sleep_ms(wait);
    if (!wifi_.connected()) {
      pair_error_ = "Pocket lost its Wi-Fi connection.";
      break;
    }
    const std::string code = cloud_.create_pair_session(cfg_.device_id);
    if (!code.empty()) {
      pair_code_ = code;
      pair_expires_ms_ = now_ms_ + 10 * 60 * 1000;
      pair_status_ = "pending";
      last_pair_poll_ms_ = 0;
      return true;
    }
  }
  pair_code_.clear();
  pair_status_ = "expired";
  if (pair_error_.empty()) pair_error_ = cloud_.last_error();
  if (pair_error_.empty()) pair_error_ = "Pocket couldn't reach Pocket Cloud.";
  error_msg_ = "Couldn't get a pairing code \xE2\x80\x94 " + pair_error_;
  error_until_ms_ = now_ms_ + 4000;
  return false;
}

void App::after_nav(bool full_refresh) {
  status_msg_.clear();  // a finished blocking step never follows the user to the next screen
  focus_.index = 0;
  list_focus_spin_count_ = 0;
  // Fresh screen: clear PIN spin latch so a prior slot's digit can't leak in.
  pin_digit_working_ = '0';
  pin_spun_since_focus_ = false;
  pin_ignore_select_until_ms_ = 0;
  if (nav_.current() == ScreenId::OnboardingWelcome && !welcome_sound_played_) {
    welcome_sound_played_ = true;
    play_sound(SoundId::Welcome);
  }
  dirty_ = true;
  if (full_refresh) {
    dirty_kind_ = DirtyKind::FullCanvas;
    refresh_.on_screen_enter_full();
    redraw(true);
  } else {
    // Fast 0xFF window of the whole panel — not the 1.5 s 0xD7 LUT.
    dirty_kind_ = DirtyKind::FullCanvas;
    redraw(false);
  }
}

void App::after_nav() { after_nav(screen_requires_full_enter(nav_.current())); }

bool PlatformCloud::download_to_file(const char* kind, const std::string& device_id, const std::string& id,
                                     const std::string& path, int64_t max_bytes, int64_t* bytes_out) {
  const std::vector<uint8_t> bytes =
      std::string(kind) == "music" ? music_download(device_id, id) : book_download(device_id, id);
  if (bytes.empty()) return false;
  if (max_bytes > 0 && static_cast<int64_t>(bytes.size()) > max_bytes) return false;
  const std::string part = path + ".part";
  FILE* f = std::fopen(part.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  std::fclose(f);
  if (!ok) {
    std::remove(part.c_str());
    return false;
  }
  std::remove(path.c_str());
  if (std::rename(part.c_str(), path.c_str()) != 0) return false;
  if (bytes_out) *bytes_out = static_cast<int64_t>(bytes.size());
  return true;
}

void App::cancel_capture() {
  if (!ptt_active_ && !mic_capturing_) return;
  ptt_active_ = false;
  mic_capturing_ = false;
  if (audio_) (void)audio_->stop_capture();  // discard — the user left mid-recording
}

void App::go_home() {
  // Already on Home — keep focus; a remount flash feels like navigation jank.
  if (nav_.current() == ScreenId::Home && nav_.depth == 1) {
    return;
  }
  cancel_capture();
  nav_.reset(ScreenId::Home);
  home_focus_spin_count_ = 0;
  list_focus_spin_count_ = 0;
  after_nav();
}

void App::go_lock() {
  // SoftAP / Add-network must stay on-screen — idle, Power (RF glitch), and Cloud
  // force_lock all funnel here. Refuse until the session ends (Cancel / success).
  if (wifi_add_flow_active()) {
    now_ms_ = clock_.now_ms();
    last_input_ms_ = now_ms_;
    return;
  }
  cancel_capture();
  // A pending SD prompt is abandoned by locking; re-arm hot-plug detection so the
  // gate can appear again (it used to stay "active" until reboot).
  if (sd_gate_active_) {
    sd_gate_active_ = false;
    sd_waiting_eject_ = false;
  }
  pin_change_ = false;
  // An app's toast must not follow the user onto the PIN screen.
  error_msg_.clear();
  error_until_ms_ = 0;
  status_msg_.clear();
  reset_armed_ = false;
  nav_.reset(ScreenId::Lock);
  pin_entry_.clear();
  parental_session_unlocked_ = false;
  parental_pin_for_app_ = false;
  if (cfg_.screensaver >= 1 && cfg_.screensaver <= kScreensaverCount) {
    lock_motif_index_ = cfg_.screensaver - 1;
  } else {
    lock_motif_index_ = (lock_motif_index_ + 1) % kLockMotifCount;
  }
  lock_seed_ = lock_seed_ * 1664525u + 1013904223u + (clock_.now_ms() & 0xFFFFu);
  // The lock face sits for hours: a deep, temperature-compensated refresh wipes all ghosting.
  want_deep_ = true;
  after_nav(true);
}

bool App::parental_requires_pin(HomeApp app) const {
  if (!home_app_is_real(app)) return false;
  if (app == HomeApp::Settings || app == HomeApp::Update) return false;
  return (cfg_.parental_pin_gated & (1u << static_cast<uint8_t>(app))) != 0;
}

void App::launch_home_app(HomeApp launch) {
  switch (launch) {
    case HomeApp::Notes:
      notes_tab_ = 0;
      nav_.push(ScreenId::NotesList);
      break;
    case HomeApp::Today:
      focus_.index = 0;
      maybe_fetch_calendar(false);  // at most every 15 min; the cached events show offline
      nav_.push(ScreenId::Today);
      break;
    case HomeApp::Ledger:
      nav_.push(ScreenId::LedgerComingSoon);
      break;
    case HomeApp::Clock:
      clock_tab_ = 0;
      nav_.push(ScreenId::ClockFace);
      break;
    case HomeApp::Pass:
      nav_.push(ScreenId::PassList);
      break;
    case HomeApp::Weather:
      weather_ensure_city();
      focus_.index = 0;
      // Fetch after the screen paints when the forecast is missing or over an hour old.
      weather_refresh_pending_ =
          wifi_.connected() &&
          (data_.weather.days.empty() || data_.weather.fetched_at < 1000000000LL || !clock_.time_valid() ||
           std::time(nullptr) - static_cast<time_t>(data_.weather.fetched_at) > 3600);
      nav_.push(ScreenId::WeatherMain);
      break;
    case HomeApp::Music:
      music_index_ = 0;
      music_load_local();  // always refresh local library (works offline)
      nav_.push(ScreenId::MusicList);
      break;
    case HomeApp::Reading:
      book_index_ = 0;
      reading_load_local();  // always refresh local library (works offline)
      nav_.push(ScreenId::ReadingList);
      break;
    case HomeApp::Settings:
      nav_.push(ScreenId::SettingsRoot);
      break;
    case HomeApp::Update:
      begin_firmware_update();
      return;
    default:
      return;
  }
  after_nav();
  // Opening Notes or Lists pulls Companion edits first (and pushes ours), visibly.
  if (launch == HomeApp::Notes && cloud_sync_possible() &&
      (last_sync_ok_ms_ == 0 || now_ms_ - last_sync_ok_ms_ > 20000u)) {
    if (cloud_sync(true)) mark_content_dirty();
  }
}

void App::apply_cloud_offline_grace() {
  if (!cfg_.cloud_entitled) return;
  if (cfg_.cloud_last_ok_unix == 0) return;
  // Only expire on a trusted wall clock — Approximate restore after power-loss can
  // lag by the offline interval and would falsely burn the 72h grace.
  if (!clock_.time_trusted()) return;
  const uint32_t now_unix = static_cast<uint32_t>(std::time(nullptr));
  if (now_unix < cfg_.cloud_last_ok_unix) return;  // clock skew / boot stamp
  if (now_unix - cfg_.cloud_last_ok_unix > kCloudOfflineGraceSec) {
    // Past grace — keep last status string but clear entitlement for outgoing Cloud use.
    cfg_.cloud_entitled = false;
    if (cfg_.cloud_status == "trialing" || cfg_.cloud_status == "active") {
      cfg_.cloud_status = "lapsed";
    }
    store_.save(cfg_);
  }
}

void App::ensure_device_token() {
  if (device_token_attempted_ || !cfg_.device_token.empty()) return;
  if (!cfg_.companion_linked || !wifi_.connected() || cfg_.device_id.empty()) return;
  device_token_attempted_ = true;  // once per boot; a failure just retries next boot
  bool already = false;
  const std::string tok = cloud_.claim_device_token(cfg_.device_id, &already);
  if (!tok.empty()) {
    cfg_.device_token = tok;
    cloud_.set_device_token(tok);
    store_.save(cfg_);
  } else if (already) {
    device_token_relink_needed_ = true;
  }
}

void App::maybe_cloud_attest() {
  if (!cfg_.onboarding_complete || !cfg_.companion_linked) return;
  if (!wifi_.connected() || cfg_.device_id.empty()) {
    apply_cloud_offline_grace();
    return;
  }
  ensure_device_token();
  // ~20s so Companion “Online” / rename stay fresh after reconnect.
  // While locked nobody is looking: check in every 10 min instead of every 20 s
  // (still picks up remote lock / Wi-Fi hand-offs, at a fraction of the radio time).
  uint32_t attest_every = nav_.current() == ScreenId::Lock ? 600000u : 20000u;
  if (battery_saver_) attest_every *= 3;  // low battery: check in less often
  // A card inserted/removed is reported right away so Companion's upload limits follow it.
  const int sd_now = sd_present() ? 1 : 0;
  const bool sd_changed = sd_now != last_reported_sd_;
  if (!sd_changed && last_attest_ms_ != 0 && now_ms_ - last_attest_ms_ < attest_every) return;
  last_reported_sd_ = sd_now;
  last_attest_ms_ = now_ms_;
  cloud_.set_sd_present(sd_present() ? 1 : 0);
  const std::string body = cloud_.device_attest_json(cfg_.device_id);
  if (body.empty()) {
    apply_cloud_offline_grace();
    return;
  }

  // Parse once; look keys up depth-first in document order (same precedence as the
  // old substring search, but quotes/escapes inside values can no longer break it).
  JsonValue attest_root;
  const bool attest_ok = json_parse(body, attest_root);
  std::function<const JsonValue*(const JsonValue&, const char*)> deep = [&](const JsonValue& v,
                                                                           const char* key) -> const JsonValue* {
    if (v.is_object()) {
      for (const auto& m : v.members) {
        if (m.first == key) return &m.second;
        if (const JsonValue* r = deep(m.second, key)) return r;
      }
    } else if (v.is_array()) {
      for (const auto& it : v.items)
        if (const JsonValue* r = deep(it, key)) return r;
    }
    return nullptr;
  };
  auto json_bool = [&](const std::string&, const char* key) -> int {
    const JsonValue* v = attest_ok ? deep(attest_root, key) : nullptr;
    return (v && v->type == JsonValue::Type::Bool) ? (v->boolean ? 1 : 0) : -1;
  };
  auto json_str = [&](const std::string&, const char* key) -> std::string {
    const JsonValue* v = attest_ok ? deep(attest_root, key) : nullptr;
    return (v && v->type == JsonValue::Type::String) ? v->str : std::string();
  };

  const int entitled = json_bool(body, "cloud_entitled");
  if (entitled >= 0) cfg_.cloud_entitled = entitled == 1;
  const std::string st = json_str(body, "status");
  if (!st.empty()) cfg_.cloud_status = st;
  // Successful attest → refresh offline grace clock (unix when wall clock trusted).
  if (clock_.time_trusted()) {
    {
      const uint32_t t = static_cast<uint32_t>(std::time(nullptr));
      if (t >= cfg_.cloud_last_ok_unix + 3600u || t < cfg_.cloud_last_ok_unix) cfg_.cloud_last_ok_unix = t;
    }
  } else {
    {
      const uint32_t t = static_cast<uint32_t>(now_ms_ / 1000u);
      if (t >= cfg_.cloud_last_ok_unix + 3600u || t < cfg_.cloud_last_ok_unix) cfg_.cloud_last_ok_unix = t;
    }
  }
  const std::string dname = json_str(body, "device_name");
  if (!dname.empty() && dname != cfg_.device_name) {
    cfg_.device_name = dname;
  }
  if (json_bool(body, "ring") == 1) ring_find_my_pocket();
  if (json_bool(body, "backup_now") == 1) backup_requested_ = true;  // "Back up now" in Companion
  // Home order chosen in Companion (keys → HomeApp indices). Empty array = default order.
  if (const JsonValue* ho = attest_ok ? deep(attest_root, "home_order") : nullptr; ho && ho->is_array()) {
    static const struct { const char* key; HomeApp app; } kKeys[] = {
        {"today", HomeApp::Today},     {"notes", HomeApp::Notes},       {"clock", HomeApp::Clock},
        {"weather", HomeApp::Weather}, {"music", HomeApp::Music},       {"reading", HomeApp::Reading},
        {"pass", HomeApp::Pass},       {"settings", HomeApp::Settings}, {"update", HomeApp::Update}};
    std::vector<uint8_t> order;
    for (const auto& it : ho->items) {
      if (it.type != JsonValue::Type::String) continue;
      for (const auto& k : kKeys)
        if (it.str == k.key && order.size() < 16) order.push_back(static_cast<uint8_t>(k.app));
    }
    if (order != cfg_.home_order) {
      cfg_.home_order = order;
      if (nav_.current() == ScreenId::Home) mark_content_dirty();
    }
  }
  // Optional lock/sleep face message — Companion is the source of truth once linked.
  const size_t lm_key = body.find("\"lock_message\"");
  if (lm_key != std::string::npos) {
    const std::string lmsg = json_str(body, "lock_message");
    if (lmsg != cfg_.lock_message) cfg_.lock_message = lmsg;
  }
  // Volume / brightness — either side can change these (rotary on-device, Companion remotely);
  // whichever changed most recently wins on the next pull since the pusher writes through too.
  auto json_int = [&](const std::string&, const char* key) -> int {
    const JsonValue* v = attest_ok ? deep(attest_root, key) : nullptr;
    return (v && v->type == JsonValue::Type::Number) ? static_cast<int>(v->number) : -1;
  };
  const int remote_vol = json_int(body, "volume_percent");
  if (remote_vol >= 0 && remote_vol <= 100 && remote_vol != static_cast<int>(cfg_.volume_percent)) {
    cfg_.volume_percent = static_cast<uint8_t>(remote_vol);
    if (audio_) audio_->set_volume(cfg_.volume_percent);
  }
  const int remote_bright = json_int(body, "brightness_percent");
  if (remote_bright >= 0 && remote_bright <= 100 && remote_bright != static_cast<int>(cfg_.brightness_percent)) {
    cfg_.brightness_percent = static_cast<uint8_t>(remote_bright);
    display_.set_brightness(cfg_.brightness_percent);
  }

  // Parental pin_gated_apps: ["notes","music",...]
  uint16_t gated = 0;
  const size_t arr = body.find("\"pin_gated_apps\"");
  if (arr != std::string::npos) {
    size_t lb = body.find('[', arr);
    size_t rb = body.find(']', lb == std::string::npos ? arr : lb);
    if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
      const std::string list = body.substr(lb, rb - lb + 1);
      auto has = [&](const char* name) { return list.find(name) != std::string::npos; };
      if (has("notes")) gated |= 1u << static_cast<uint8_t>(HomeApp::Notes);
      if (has("ledger")) gated |= 1u << static_cast<uint8_t>(HomeApp::Ledger);
      if (has("clock")) gated |= 1u << static_cast<uint8_t>(HomeApp::Clock);
      if (has("pass")) gated |= 1u << static_cast<uint8_t>(HomeApp::Pass);
      if (has("weather")) gated |= 1u << static_cast<uint8_t>(HomeApp::Weather);
      if (has("music")) gated |= 1u << static_cast<uint8_t>(HomeApp::Music);
    }
  }
  cfg_.parental_pin_gated = gated;
  const int hide_pass = json_bool(body, "hide_pass_share");
  if (hide_pass >= 0) cfg_.parental_hide_pass_share = hide_pass == 1;
  const int block_conn = json_bool(body, "block_connectors");
  if (block_conn >= 0) cfg_.parental_block_connectors = block_conn == 1;
  const int force_lock = json_bool(body, "force_lock");
  if (force_lock == 1 && nav_.current() != ScreenId::Lock && nav_.current() != ScreenId::Pin) {
    go_lock();
  }

  // Pending Wi‑Fi from Companion (in-app, no SoftAP hop).
  const std::string pending_ssid = json_str(body, "ssid");
  // pending_wifi object nests ssid — look near pending_wifi key
  std::string wifi_ssid;
  std::string wifi_pass;
  if (attest_ok) {
    if (const JsonValue* pw = attest_root.get("pending_wifi"); pw && pw->is_object()) {
      wifi_ssid = pw->get_string("ssid");
      wifi_pass = pw->get_string("password");
    }
  }
  (void)pending_ssid;
  if (!wifi_ssid.empty()) {
    connect_and_remember(wifi_ssid, wifi_pass);
  }

  store_.save(cfg_);
  mark_status_dirty();
}

void App::notify_wifi_connected() {
  last_attest_ms_ = 0;
  maybe_cloud_attest();
  last_pending_stt_ms_ = 0;
  flush_pending_dictation();
  mark_status_dirty();
}

bool App::wifi_add_flow_active() const {
  if (wifi_add_session_) return true;
  if (wifi_softap_start_pending_) return true;
  if (wifi_rf_guard_active()) return true;
  if (wifi_sta_connecting_) return true;
  if (wifi_.provisioning()) return true;
  switch (nav_.current()) {
    case ScreenId::OnboardingWifiPassword:
    case ScreenId::OnboardingWifiList:
    case ScreenId::OnboardingWifiConnecting:
      return true;
    case ScreenId::SettingsWifi:
      // Add-network chooser (Personal Hotspot / Another Wi‑Fi) — stay unlocked.
      return wifi_ui_page_ == 1;
    default:
      return false;
  }
}

void App::tick(uint32_t now_ms) {
  now_ms_ = now_ms;
  maybe_start_pending_softap_radio();
  maybe_tick_clock_events();
  // Track finished on its own → Now Playing shows Stopped.
  if (music_playing_ && audio_ && !audio_->is_playing()) {
    music_playing_ = false;
    if (nav_.current() == ScreenId::MusicNowPlaying) mark_content_dirty();
  }
  // Low battery: warn once at 10% and again at 5% (re-arms after charging past 20%).
  if (cfg_.onboarding_complete && (last_battery_check_ms_ == 0 || now_ms_ - last_battery_check_ms_ >= 60000u)) {
    last_battery_check_ms_ = now_ms_;
    const int b = clock_.battery_percent();
    if (b >= 20) battery_warned_ = 100;
    const int level = b <= 5 ? 5 : (b <= 10 ? 10 : 100);
    if (b >= 0 && level < battery_warned_ && nav_.current() != ScreenId::Lock &&
        nav_.current() != ScreenId::ClockRinging) {
      battery_warned_ = level;
      error_msg_ = level == 5 ? "Battery critically low \xE2\x80\x94 charge Pocket now."
                              : "Battery low \xE2\x80\x94 charge Pocket soon.";
      error_until_ms_ = now_ms_ + 5000;
      transient_error_armed_ = true;
      play_sound(SoundId::Attention);
      mark_content_dirty();
    }
  }
  if (weather_refresh_pending_ && nav_.current() == ScreenId::WeatherMain && !dirty_ && !display_.busy()) {
    weather_refresh_pending_ = false;
    weather_refresh();
  }
  // Never idle-lock mid SoftAP / Add network — phone needs time to join Pocket Wi‑Fi.
  // Guard now_ms < last_input_ms_ (unsigned underflow): host pumps often tick(0) while
  // handle() samples the real clock, and a wrap must not force Lock.
  if (cfg_.onboarding_complete && nav_.current() != ScreenId::Lock && nav_.current() != ScreenId::Pin &&
      nav_.current() != ScreenId::ClockRinging && !wifi_add_flow_active() && !keeps_awake() &&
      now_ms >= last_input_ms_ && now_ms - last_input_ms_ >= idle_lock_after_ms()) {
    go_lock();
    return;
  }
  maybe_setup_rejoin();
  // Lock plate: drop an expired capture message; keep the info line current (new day,
  // alarm changed, fresh forecast) with a small plate-only partial — never the art.
  if (nav_.current() == ScreenId::Lock && !dirty_ && !display_.busy()) {
    auto plate_dirty = [&]() {
      if (cfg_.screensaver == kScreensaverPlain) mark_content_dirty();
      else mark_region_dirty(0, 680, kCanvasW, kCanvasH - 680);
    };
    if (lock_flash_until_ms_ != 0 && now_ms_ >= lock_flash_until_ms_) {
      lock_flash_until_ms_ = 0;
      plate_dirty();
    } else if (cfg_.lock_info && (last_lock_info_ms_ == 0 || now_ms_ - last_lock_info_ms_ >= 60000u)) {
      last_lock_info_ms_ = now_ms_;
      if (lock_info_line() != lock_info_shown_) plate_dirty();
    }
  }
  maybe_push_reading_progress(false);
  maybe_battery_saver();
  maybe_backup(false);
  if (nav_.current() == ScreenId::Lock && !dirty_ && !display_.busy() && quiet_now() != lock_night_shown_ &&
      (last_lock_info_ms_ == now_ms_)) {
    // Quiet hours just began or ended: switch faces with a deep, clean refresh.
    want_deep_ = true;
    redraw(true);
  }
  if ((nav_.current() == ScreenId::Home || nav_.current() == ScreenId::Lock) && !dirty_ && !display_.busy())
    maybe_fetch_calendar(false);
  // SoftAP Wi‑Fi provision: wait for phone to POST credentials
  if (nav_.current() == ScreenId::OnboardingWifiPassword && !wifi_sta_connecting_) {
    if (now_ms - last_wifi_prov_poll_ms_ >= 400) {
      last_wifi_prov_poll_ms_ = now_ms;
      std::string ssid;
      std::string pass;
      if (wifi_.take_provision_credentials(&ssid, &pass)) {
        wifi_sta_connecting_ = true;
        error_msg_.clear();
        mark_content_dirty();
        // Stay on SoftAP screen — SoftAP stays up (APSTA) during STA attempt.
        const bool ok = connect_and_remember(ssid, pass);
        wifi_sta_connecting_ = false;
        if (ok) {
          wifi_add_session_ = false;
          wifi_softap_start_pending_ = false;
          wifi_rf_guard_until_ms_ = 0;
          if (cfg_.onboarding_complete) {
            focus_.index = 0;
            wifi_ui_page_ = 0;
            nav_.return_to(ScreenId::SettingsWifi);
            after_nav();
          } else {
            // Online: always move on to Link. If the code couldn't be fetched, that screen
            // says why and offers Try again — never back to the (now closed) setup network.
            play_sound(mint_pair_session() ? SoundId::Success : SoundId::Attention);
            focus_.index = 0;
            nav_.replace(ScreenId::OnboardingCompanionQr);
            after_nav();  // fast refresh — a full flash while the radio works risks a brownout
          }
        } else {
          const std::string why = wifi_.last_error();
          error_msg_ = why.empty() ? std::string("Couldn't connect. Check the password on your phone.") : why;
          error_until_ms_ = now_ms_ + 4000;
          ensure_softap_credentials_shown();
          mark_content_dirty();
        }
      }
    }
  }
  // Pairing poll (throttle HTTP — UI loop is ~50ms)
  if (nav_.current() == ScreenId::OnboardingCompanionQr && !pair_code_.empty()) {
    if (now_ms > pair_expires_ms_) {
      if (pair_status_ != "expired") {
        pair_status_ = "expired";
        mark_content_dirty();
      }
    } else if (now_ms - last_pair_poll_ms_ >= 2500) {
      last_pair_poll_ms_ = now_ms;
      auto st = cloud_.pair_status(pair_code_);
      if (st == "claimed") {
        pair_status_ = "claimed";
        cfg_.companion_linked = true;
        // A fresh link brings this Pocket its own token (older links collect one later).
        const std::string tok = cloud_.take_pair_device_token();
        if (!tok.empty()) {
          cfg_.device_token = tok;
          cloud_.set_device_token(tok);
          device_token_relink_needed_ = false;
        }
        store_.save(cfg_);
        play_sound(SoundId::Success);
        // Companion required: advance as soon as linked (no Skip path).
        if (!cfg_.onboarding_complete) {
          focus_.index = 0;
          // A new Pocket on an account with a backup offers to restore it first.
          nav_.replace(offer_restore_if_any() ? ScreenId::OnboardingRestore : ScreenId::OnboardingPinLength);
          after_nav();
        } else {
          mark_content_dirty();
        }
      } else if (st == "expired" && pair_status_ != "expired") {
        pair_status_ = "expired";
        mark_content_dirty();
      }
    }
  }
  // SD eject wait (setup SoftAP path or anytime gate)
  if ((nav_.current() == ScreenId::OnboardingSdCard || nav_.current() == ScreenId::SdCardGate) &&
      sd_waiting_eject_) {
    if (now_ms - last_sd_poll_ms_ >= 500) {
      last_sd_poll_ms_ = now_ms;
      const bool still = storage_ && storage_->present();
      if (!still) {
        sd_waiting_eject_ = false;
        play_sound(SoundId::Click);
        if (nav_.current() == ScreenId::SdCardGate) {
          sd_was_present_ = false;
          leave_sd_gate();
        } else {
          begin_softap_link();
        }
      }
    }
  }
  if (ptt_active_ && mic_capturing_ && audio_) audio_->poll_capture();
  maybe_poll_sd_hotplug();
  maybe_wifi_auto_reconnect();
  maybe_cloud_attest();
  maybe_cloud_sync();
  // Retry queued offline voice clips (cloud STT) — notify_wifi covers the fast path.
  if (wifi_.connected() && notes_have_pending_dictation() &&
      (last_pending_stt_ms_ == 0 || now_ms - last_pending_stt_ms_ >= 60000u)) {
    last_pending_stt_ms_ = now_ms;
    flush_pending_dictation();
  }
  if (dirty_ && try_present(false)) {
    dirty_ = false;
  }
  maybe_tick_pin_chrome();
  maybe_tick_transient_error();
  maybe_tick_status_chrome();
  maybe_tick_clock_face();
  maybe_tick_home_clock();
}

void App::present_canvas(bool full) {
  last_present_ms_ = now_ms_;
  canvas_.clear(Gray::G3);
  render();
  if (!status_msg_.empty()) ui::toast(canvas_, status_msg_);
  if (full) {
    const RefreshMode m = want_deep_ ? RefreshMode::Deep : RefreshMode::Full;
    want_deep_ = false;
    display_.present(canvas_, m);
    refresh_.on_applied(m);
    dirty_kind_ = DirtyKind::FullCanvas;
    return;
  }
  RefreshMode mode = refresh_.plan(true);
  if (mode == RefreshMode::Full) {
    display_.present(canvas_, RefreshMode::Full);
    refresh_.on_applied(mode);
    dirty_kind_ = DirtyKind::FullCanvas;
    return;
  }
  if (dirty_kind_ == DirtyKind::StatusBar) {
    display_.present_region(canvas_, 0, 0, kCanvasW, kStatusBarH);
    refresh_.on_applied_tiny(RefreshMode::Partial);
  } else if (dirty_kind_ == DirtyKind::Region) {
    display_.present_region(canvas_, dirty_rx_, dirty_ry_, dirty_rw_, dirty_rh_);
    if (dirty_rh_ <= kStatusBarH + 40) {
      refresh_.on_applied_tiny(RefreshMode::Partial);
    } else {
      refresh_.on_applied(RefreshMode::Partial);
    }
  } else if (dirty_kind_ == DirtyKind::FullCanvas) {
    // Whole-panel 0xFF partial (~0.3 s) — fastest valid update that still paints everything.
    display_.present_region(canvas_, 0, 0, kCanvasW, kCanvasH);
    refresh_.on_applied(RefreshMode::Partial, RefreshPolicy::kScreenChangeWeight);
  } else {
    // ContentBand (Home/list focus): below status bar. Count as half a partial so
    // ~16 focus spins don't surprise-trigger a slow 0xD7 mid-browse (v97).
    display_.present_region(canvas_, 0, kStatusBarH, kCanvasW, kCanvasH - kStatusBarH);
    refresh_.on_applied_band();
  }
  dirty_kind_ = DirtyKind::FullCanvas;
}

void App::redraw(bool full) {
  if (full) {
    dirty_kind_ = DirtyKind::FullCanvas;
    want_full_ = true;  // survives a busy panel: the deferred flush stays a full refresh
  }
  dirty_ = true;
  // Lock→PIN (and every after_nav) used to clear dirty even when present was
  // skipped for busy — screen stayed on Lock with dead-looking buttons.
  if (!try_present(full || want_full_)) return;
  dirty_ = false;
  want_full_ = false;
}

#ifdef POCKET_HOST
void App::host_goto(ScreenId s, int tab) {
  nav_.reset(ScreenId::Home);
  if (s == ScreenId::Lock) nav_.reset(ScreenId::Lock);
  else if (s != ScreenId::Home) nav_.push(s);
  focus_.index = 0;
  notes_tab_ = tab;
  clock_tab_ = tab;
  if (s == ScreenId::Pin) pin_enter_empty_slot();
  dirty_kind_ = DirtyKind::FullCanvas;
}
#endif

void App::draw_status_bar() {
  int h = 0, m = 0, wd = 0, mo = 0, d = 0;
  clock_.local_hm(h, m, wd, mo, d);
  const bool wifi_ok = wifi_.connected() || wifi_.provisioning();
  draw_status_bar_impl(canvas_, cfg_, h, m, wifi_ok, clock_.battery_percent(), clock_.time_valid(),
                       clock_.time_needs_network_sync());
  if (battery_saver_) {
    const int ty = (kStatusBarH - canvas_.text_height(Canvas::TextRole::StatusBar)) / 2;
    canvas_.draw_text_centered(kCanvasW / 2, ty, "Battery saver", Canvas::TextRole::StatusBar, Gray::G0);
  }
}

void App::handle(InputEvent e) {
  if (e == InputEvent::None) return;
  // Always sample the clock — now_ms_ may be stale if the previous tick was long ago
  // or a prior handle() blocked in SoftAP / e-ink.
  now_ms_ = clock_.now_ms();
  last_input_ms_ = now_ms_;

  if (e == InputEvent::Home) {
    // SoftAP add/link — stay on credentials until Cancel (long-press Home would strand SoftAP).
    if (wifi_add_flow_active()) return;
    if (!long_home_blocked(nav_.current(), cfg_.onboarding_complete)) {
      go_home();
    }
    return;
  }
  if (e == InputEvent::PowerHold) {
    // Still holding: say what happens if they keep going (clears on release / lock).
    const ScreenId cur = nav_.current();
    if (cfg_.onboarding_complete && !wifi_add_flow_active() && cur != ScreenId::Lock && cur != ScreenId::Pin &&
        cur != ScreenId::ClockRinging && cur != ScreenId::SettingsUpdateProgress)
      show_status("Keep holding to lock");
    return;
  }
  if (e == InputEvent::Power || e == InputEvent::PowerLong) {
    // Setup incomplete: stay on onboarding — do not jump to Lock.
    if (!cfg_.onboarding_complete) return;
    // SoftAP RF often glitches PWR → ignore Power mid Add-network / SoftAP session.
    if (wifi_add_flow_active()) return;
    const ScreenId cur = nav_.current();
    if (cur == ScreenId::ClockRinging) {
      stop_ringing(false);  // either press silences an alarm
      if (e == InputEvent::PowerLong) go_lock();
      return;
    }
    if (cur == ScreenId::SettingsUpdateProgress) return;  // never interrupt an install
    if (e == InputEvent::PowerLong) {
      // Hold → lock.
      if (cur != ScreenId::Lock && cur != ScreenId::Pin) go_lock();
      return;
    }
    // Tap → Home. From the lock face it opens PIN entry (Home is behind the PIN).
    if (cur == ScreenId::Lock) {
      handle_lock(InputEvent::Select);
    } else if (cur != ScreenId::Pin) {
      go_home();
    }
    return;
  }
  // SoftAP RF also glitches BOOT → Back. During Add-network SoftAP, only Cancel clears
  // the session — a spurious Back would drop wifi_add_session_ then Power→Lock.
  if (e == InputEvent::Back && cfg_.onboarding_complete &&
      (wifi_add_session_ || wifi_softap_start_pending_ || wifi_rf_guard_active()) &&
      nav_.current() == ScreenId::OnboardingWifiPassword) {
    return;
  }

  switch (nav_.current()) {
    case ScreenId::Lock:
      handle_lock(e);
      break;
    case ScreenId::Pin:
      handle_pin(e);
      break;
    case ScreenId::OnboardingWelcome:
    case ScreenId::OnboardingCompanionDownload:
    case ScreenId::OnboardingSdCard:
    case ScreenId::OnboardingWifiList:
    case ScreenId::OnboardingWifiPassword:
    case ScreenId::OnboardingWifiConnecting:
    case ScreenId::OnboardingCompanionQr:
    case ScreenId::OnboardingPinLength:
    case ScreenId::OnboardingPinSet:
    case ScreenId::OnboardingPinConfirm:
    case ScreenId::OnboardingTimezone:
    case ScreenId::OnboardingMicTest:
    case ScreenId::OnboardingDone:
      handle_onboarding(e);
      break;
    case ScreenId::SdCardGate:
      handle_sd_gate(e);
      break;
    case ScreenId::Home:
      handle_home(e);
      break;
    case ScreenId::NotesList:
    case ScreenId::NotesDetail:
    case ScreenId::ListsList:
    case ScreenId::ListsDetail:
      handle_notes(e);
      break;
    case ScreenId::LedgerComingSoon:
      handle_ledger(e);
      break;
    case ScreenId::ClockFace:
    case ScreenId::ClockAlarms:
    case ScreenId::ClockAlarmEdit:
    case ScreenId::ClockTimers:
    case ScreenId::ClockTimerRun:
    case ScreenId::ClockRinging:
      handle_clock(e);
      break;
    case ScreenId::SettingsUpdateProgress:
    case ScreenId::SettingsUpdateResult:
    case ScreenId::SettingsUpdateAvailable:
      handle_update(e);
      break;
    case ScreenId::PassList:
    case ScreenId::PassDetail:
      handle_pass(e);
      break;
    case ScreenId::WeatherMain:
    case ScreenId::WeatherCitySetup:
      handle_weather(e);
      break;
    case ScreenId::MusicList:
    case ScreenId::MusicNowPlaying:
      handle_music(e);
      break;
    case ScreenId::ReadingList:
    case ScreenId::ReadingBook:
      handle_reading(e);
      break;
    case ScreenId::Today:
      handle_today(e);
      break;
    case ScreenId::OnboardingRestore:
      handle_restore(e);
      break;
    default:
      handle_settings(e);
      break;
  }
  if (dirty_ && !defer_handle_present_ && try_present(false)) {
    // Preserve dirty_kind_ (Region / StatusBar) — do not force FullCanvas.
    dirty_ = false;
  }
}

void App::render() {
  switch (nav_.current()) {
    case ScreenId::Lock:
      render_lock();
      break;
    case ScreenId::Pin:
      render_pin();
      break;
    case ScreenId::OnboardingWelcome:
    case ScreenId::OnboardingCompanionDownload:
    case ScreenId::OnboardingSdCard:
    case ScreenId::OnboardingWifiList:
    case ScreenId::OnboardingWifiPassword:
    case ScreenId::OnboardingWifiConnecting:
    case ScreenId::OnboardingCompanionQr:
    case ScreenId::OnboardingPinLength:
    case ScreenId::OnboardingPinSet:
    case ScreenId::OnboardingPinConfirm:
    case ScreenId::OnboardingTimezone:
    case ScreenId::OnboardingMicTest:
    case ScreenId::OnboardingDone:
      render_onboarding();
      break;
    case ScreenId::SdCardGate:
      render_sd_gate();
      break;
    case ScreenId::Home:
      render_home();
      break;
    case ScreenId::NotesList:
    case ScreenId::NotesDetail:
    case ScreenId::ListsList:
    case ScreenId::ListsDetail:
      render_notes();
      break;
    case ScreenId::LedgerComingSoon:
      render_ledger();
      break;
    case ScreenId::ClockFace:
    case ScreenId::ClockAlarms:
    case ScreenId::ClockAlarmEdit:
    case ScreenId::ClockTimers:
    case ScreenId::ClockTimerRun:
    case ScreenId::ClockRinging:
      render_clock();
      break;
    case ScreenId::SettingsUpdateProgress:
    case ScreenId::SettingsUpdateResult:
    case ScreenId::SettingsUpdateAvailable:
      render_update();
      break;
    case ScreenId::Today:
      render_today();
      break;
    case ScreenId::OnboardingRestore:
      render_restore();
      break;
    case ScreenId::PassList:
    case ScreenId::PassDetail:
      render_pass();
      break;
    case ScreenId::WeatherMain:
    case ScreenId::WeatherCitySetup:
      render_weather();
      break;
    case ScreenId::MusicList:
    case ScreenId::MusicNowPlaying:
      render_music();
      break;
    case ScreenId::ReadingList:
    case ScreenId::ReadingBook:
      render_reading();
      break;
    default:
      render_settings();
      break;
  }
}

// --- Lock / PIN -----------------------------------------------------------

void App::render_lock() {
  // Sleep face: a full-bleed generative print over a quiet paper plate. Static —
  // no clock, no hints — so the panel can hold it with zero refreshes.
  constexpr int kArtH = 680;
  if (show_update_welcome_) {
    // First boot after an update: a calm welcome card. One press → normal lock face.
    ui::waves(canvas_, 50, 190);
    canvas_.draw_text_centered(kCanvasW / 2, 290, "Update complete", Canvas::TextRole::Caption, Gray::G0);
    canvas_.draw_text_centered(kCanvasW / 2, 322, "Welcome to", Canvas::TextRole::Secondary, Gray::G0);
    canvas_.draw_text_centered(kCanvasW / 2, 356, kOsDisplayName, Canvas::TextRole::ScreenTitle, Gray::G0);
    int y = 428;
    canvas_.dotted_hline(kCanvasW / 2 - 60, y, 120, Gray::G0, 3);
    y += 24;
    for (int i = 0; i < kReleaseNoteCount && y < kBottomCtaY - 70; ++i) {
      canvas_.fill_disc(kSideMargin + 6, y + 15, 3, Gray::G0);
      y = canvas_.draw_text_wrapped(kSideMargin + 22, y, kContentW - 22, 2, kReleaseNotes[i],
                                    Canvas::TextRole::Secondary, Gray::G0) + 10;
    }
    ui::button(canvas_, kBottomCtaY, "Press to continue", true);
    return;
  }
  // Quiet hours: the bedtime face (a capture or its confirmation still shows the normal plate).
  lock_night_shown_ = quiet_now() && !lock_recording_ && !(now_ms_ < lock_flash_until_ms_);
  if (lock_night_shown_) {
    render_night_face();
    return;
  }
  const bool plain = cfg_.screensaver == kScreensaverPlain;
  if (plain) {
    const int wm_w = canvas_.text_width("Pocket", Canvas::TextRole::WordMark);
    const int mark = 30;
    const int total = mark + 14 + wm_w;
    const int x0 = (kCanvasW - total) / 2;
    const int y = 330;
    draw_pocket_mark(x0 + mark, y + canvas_.text_height(Canvas::TextRole::WordMark) / 2);
    canvas_.draw_text(x0 + mark + 14, y, "Pocket", Canvas::TextRole::WordMark, Gray::G0);
    canvas_.dotted_hline(kCanvasW / 2 - 60, y + 72, 120, Gray::G0, 3);
    int ly = y + 92;
    if (!cfg_.lock_message.empty()) {
      const int tw = std::min(kContentW, canvas_.text_width(cfg_.lock_message, Canvas::TextRole::Secondary));
      canvas_.draw_text_fit((kCanvasW - tw) / 2, ly, kContentW, cfg_.lock_message, Canvas::TextRole::Secondary,
                            Gray::G0);
      ly += 34;
    }
    std::string extra;
    if (lock_recording_) extra = "Recording a voice note\xE2\x80\xA6 release to save";
    else if (now_ms_ < lock_flash_until_ms_) extra = lock_flash_;
    else if (cfg_.lock_info) extra = lock_info_shown_ = lock_info_line();
    if (!extra.empty()) {
      const int tw = std::min(kContentW, canvas_.text_width(extra, Canvas::TextRole::Caption));
      canvas_.draw_text_fit((kCanvasW - tw) / 2, ly, kContentW, extra, Canvas::TextRole::Caption, Gray::G0);
    }
    return;
  }

  draw_lock_motif();

  // Plate: centered wordmark, then up to two quiet lines — owner message, and either the
  // capture status or the at-a-glance info (date · next alarm · weather). No art credits.
  canvas_.fill_rect(0, kArtH, kCanvasW, kCanvasH - kArtH, Gray::G3);
  canvas_.hline(0, kArtH, kCanvasW, Gray::G0);
  std::vector<std::pair<std::string, Canvas::TextRole>> lines;
  if (!cfg_.lock_message.empty()) lines.push_back({cfg_.lock_message, Canvas::TextRole::Secondary});
  if (lock_recording_) {
    lines.push_back({"Recording a voice note\xE2\x80\xA6 release to save", Canvas::TextRole::BodyStrong});
  } else if (now_ms_ < lock_flash_until_ms_ && !lock_flash_.empty()) {
    lines.push_back({lock_flash_, Canvas::TextRole::BodyStrong});
  } else if (cfg_.lock_info) {
    lock_info_shown_ = lock_info_line();
    if (!lock_info_shown_.empty()) lines.push_back({lock_info_shown_, Canvas::TextRole::Caption});
  }
  const int wm_h = canvas_.text_height(Canvas::TextRole::WordMark);
  const int wm_y = kArtH + (lines.empty() ? 38 : lines.size() == 1 ? 16 : 6);
  const int wm_w = canvas_.text_width("Pocket", Canvas::TextRole::WordMark);
  const int x0 = (kCanvasW - (wm_w + 38)) / 2;
  draw_pocket_mark(x0 + 26, wm_y + wm_h / 2);
  canvas_.draw_text(x0 + 38, wm_y, "Pocket", Canvas::TextRole::WordMark, Gray::G0);
  int ly = wm_y + wm_h + 4;
  for (const auto& l : lines) {
    const int mw = std::min(kContentW, canvas_.text_width(l.first, l.second));
    canvas_.draw_text_fit((kCanvasW - mw) / 2, ly, kContentW, l.first, l.second, Gray::G0);
    ly += canvas_.text_height(l.second) + 2;
  }
}

std::string App::lock_info_line() const {
  std::string out;
  if (clock_.time_valid()) {
    static const char* kDow[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int h = 0, m = 0, wd = 0, mo = 0, d = 0;
    clock_.local_hm(h, m, wd, mo, d);
    char b[32];
    std::snprintf(b, sizeof(b), "%s, %s %d", kDow[((wd % 7) + 7) % 7], kMon[((mo % 12) + 12) % 12], d);
    out = b;
    // Next enabled alarm (today if still ahead, else the earliest one tomorrow).
    const int now = h * 60 + m;
    int best = -1, best_key = 1 << 30;
    for (const auto& al : data_.alarms) {
      if (!al.enabled) continue;
      const int key = al.minutes_of_day > now ? al.minutes_of_day - now : al.minutes_of_day + 1440 - now;
      if (key < best_key) {
        best_key = key;
        best = al.minutes_of_day;
      }
    }
    if (best >= 0) {
      const int ah = best / 60, am = best % 60;
      if (cfg_.time_format == 24) std::snprintf(b, sizeof(b), "Alarm %02d:%02d", ah, am);
      else std::snprintf(b, sizeof(b), "Alarm %d:%02d %s", ah % 12 == 0 ? 12 : ah % 12, am, ah < 12 ? "AM" : "PM");
      out += std::string(" \xC2\xB7 ") + b;
    }
  }
  // Today's forecast if it is from the last day.
  const auto& w = data_.weather;
  if (!w.days.empty() && w.fetched_at > 1000000000LL && clock_.time_valid() &&
      std::time(nullptr) - static_cast<time_t>(w.fetched_at) < 24 * 3600) {
    char t[32];
    std::snprintf(t, sizeof(t), "%d\xC2\xB0 %s", weather_display_temp(w.today_temp), w.today_condition.c_str());
    out += (out.empty() ? "" : " \xC2\xB7 ") + std::string(t);
  }
  return out;
}

/** Small geometric mark to the left of the "Pocket" wordmark: a folded-corner square,
 * echoing a pocket without being a literal icon. `right_x` is the mark's right edge. */
void App::draw_pocket_mark(int right_x, int cy) {
  constexpr int kSize = 24;
  const int x = right_x - kSize;
  const int y = cy - kSize / 2;
  canvas_.stroke_round_rect(x, y, kSize, kSize, 5, Gray::G0, 3);
  constexpr int kFold = 10;
  for (int i = 0; i < kFold; ++i) {
    canvas_.hline(x + kSize - kFold + i, y + i, kFold - i, Gray::G0);
  }
}

void App::draw_lock_motif() {
  const int piece = ((lock_motif_index_ % kLockMotifCount) + kLockMotifCount) % kLockMotifCount;
  draw_screensaver(canvas_, piece, 0, 0, kCanvasW, 680, lock_seed_);
}

void App::handle_lock(InputEvent e) {
  if (show_update_welcome_) {
    if (e == InputEvent::Select || e == InputEvent::Back) {
      show_update_welcome_ = false;
      after_nav(true);  // full refresh into the normal lock face
    }
    return;
  }
  if (e == InputEvent::PttStart && audio_ && !lock_recording_) {
    lock_recording_ = true;
    ptt_active_ = true;
    mic_capturing_ = true;
    audio_->start_capture();
    mark_region_dirty(0, 680, kCanvasW, kCanvasH - 680);
    return;
  }
  if (e == InputEvent::PttStop && lock_recording_) {
    lock_recording_ = false;
    ptt_active_ = false;
    mic_capturing_ = false;
    const MicCaptureResult cap = audio_->stop_capture();
    if (cap.ok && !cap.pcm.empty() && queue_offline_dictation(cap.pcm, /*open_note=*/false)) {
      lock_flash_ = wifi_.connected() ? "Voice note saved \xE2\x80\x94 it will appear in Notes"
                                      : "Voice note saved \xE2\x80\x94 transcribes on Wi-Fi";
      last_pending_stt_ms_ = 0;  // transcribe soon if online
      play_sound(SoundId::Success);
    } else {
      lock_flash_ = "Didn't catch that \xE2\x80\x94 hold and speak closer";
      play_sound(SoundId::Attention);
    }
    error_msg_.clear();  // the lock face says it; no toast later on another screen
    error_until_ms_ = 0;
    lock_flash_until_ms_ = now_ms_ + 5000;
    mark_region_dirty(0, 680, kCanvasW, kCanvasH - 680);
    return;
  }
  if (e == InputEvent::Select) {
    pin_entry_.clear();
    pin_spin_count_ = 0;
    pin_ignore_select_until_ms_ = 0;
    pin_enter_empty_slot();
    nav_.push(ScreenId::Pin);
    after_nav();
  }
  // Up/Down/Back/Home ignored per Spec
}

void App::draw_pin_entry(bool mask_completed, int band_top) {
  pin_draw_band_top_ = band_top;
  const int n = cfg_.pin_length;
  constexpr int kSlotW = 52;
  constexpr int kSlotH = 72;
  constexpr int kGap = 14;
  const int total = n * kSlotW + (n - 1) * kGap;
  const int x0 = (kCanvasW - total) / 2;
  const int slot_y = band_top;

  // Wipe the PIN band (incl. error/lockout) so region partials never composite stale ink.
  {
    const int wipe_top = slot_y - 44;
    canvas_.fill_rect(0, wipe_top, kCanvasW, std::min(kCanvasH - wipe_top, 460 - wipe_top), Gray::G3);
  }

  // Progress — which digit we're on (preview slot = next to fill).
  char prog[32];
  const int at = std::min(static_cast<int>(pin_entry_.size()) + 1, n);
  std::snprintf(prog, sizeof(prog), "Digit %d of %d", at, n);
  canvas_.draw_text_centered(kCanvasW / 2, slot_y - 36, prog, Canvas::TextRole::Caption, Gray::G0);

  // Fixed digit cell width so 1 vs 0 doesn't optically "jump" sideways on spin.
  int dig_box = 0;
  for (char d = '0'; d <= '9'; ++d) {
    char tmp[2] = {d, 0};
    dig_box = std::max(dig_box, canvas_.text_width(tmp, Canvas::TextRole::PinDigit));
  }

  // Invariant: focus index == committed length; focused glyph == pin_digit_working_.
  focus_.index = static_cast<int>(pin_entry_.size());

  for (int i = 0; i < n; ++i) {
    const int x = x0 + i * (kSlotW + kGap);
    const bool focused = focus_.index == i;
    const bool filled = i < static_cast<int>(pin_entry_.size());

    // Solid slot fill first — critical for e-ink partial refresh correctness.
    const int dig_y = slot_y + (kSlotH - canvas_.text_height(Canvas::TextRole::PinDigit)) / 2;
    if (focused) {
      canvas_.fill_round_rect(x, slot_y, kSlotW, kSlotH, 10, Gray::G0);
      // Always paint the live working digit (equals pin_slot_value(i)).
      char s[2] = {pin_digit_working_, 0};
      const int tw = canvas_.text_width(s, Canvas::TextRole::PinDigit);
      const int tx = x + (kSlotW - dig_box) / 2 + (dig_box - tw) / 2;
      // White digit on black focus — high contrast; flips cleanly on partials.
      canvas_.draw_text(tx, dig_y, s, Canvas::TextRole::PinDigit, Gray::G3);
    } else if (filled) {
      canvas_.fill_rect(x, slot_y, kSlotW, kSlotH, Gray::G3);
      canvas_.stroke_round_rect(x, slot_y, kSlotW, kSlotH, 10, Gray::G0, 2, Gray::G3);
      if (mask_completed) {
        const int cx = x + kSlotW / 2;
        const int cy = slot_y + kSlotH / 2;
        canvas_.fill_disc(cx, cy, 8, Gray::G0);
      } else {
        char s[2] = {pin_entry_[static_cast<size_t>(i)], 0};
        const int tw = canvas_.text_width(s, Canvas::TextRole::PinDigit);
        const int tx = x + (kSlotW - dig_box) / 2 + (dig_box - tw) / 2;
        canvas_.draw_text(tx, dig_y, s, Canvas::TextRole::PinDigit, Gray::G0);
      }
    } else {
      // Empty slot: dotted outline + baseline tick — clearly a slot, quieter than a filled one.
      canvas_.fill_rect(x, slot_y, kSlotW, kSlotH, Gray::G3);
      canvas_.stroke_round_rect(x, slot_y, kSlotW, kSlotH, 10, Gray::G0, 1, Gray::G3);
      canvas_.fill_rect(x + 16, slot_y + kSlotH - 18, kSlotW - 32, 3, Gray::G0);
    }
  }

  canvas_.draw_text_centered(kCanvasW / 2, slot_y + kSlotH + 26, "Turn to choose \xC2\xB7 Press to set",
                             Canvas::TextRole::Caption, Gray::G0);
  canvas_.draw_text_centered(kCanvasW / 2, slot_y + kSlotH + 52, "Back erases a digit",
                             Canvas::TextRole::Caption, Gray::G0);
}

void App::render_pin() {
  // Paper the whole panel under the status bar so lock/sleep motif never shows
  // through PIN chrome (partial enters used to leave motif ink in undirtied bands).
  canvas_.fill_rect(0, kStatusBarH, kCanvasW, kCanvasH - kStatusBarH, Gray::G3);
  draw_status_bar();
  {
    const int tw = canvas_.text_width("Enter PIN", Canvas::TextRole::ScreenTitle);
    const int x = (kCanvasW - tw - 40) / 2;
    ui::icon(canvas_, ui::Icon::Lock, x + 14, kContentTop + 8 + 24, 28, Gray::G0);
    canvas_.draw_text(x + 40, kContentTop + 8, "Enter PIN", Canvas::TextRole::ScreenTitle, Gray::G0);
  }
  draw_pin_entry(/*mask_completed=*/true, 200);
  if (now_ms_ < error_until_ms_) {
    const int tw = std::min(kContentW, canvas_.text_width(error_msg_, Canvas::TextRole::BodyStrong));
    canvas_.draw_text_fit((kCanvasW - tw) / 2, 392, kContentW, error_msg_, Canvas::TextRole::BodyStrong, Gray::G0);
  }
  if (now_ms_ < pin_lockout_until_ms_) {
    uint32_t rem = (pin_lockout_until_ms_ - now_ms_ + 999u) / 1000u;
    if (rem < 1) rem = 1;
    char lockbuf[40];
    if (rem == 1) {
      std::snprintf(lockbuf, sizeof(lockbuf), "Try again in 1 second");
    } else {
      std::snprintf(lockbuf, sizeof(lockbuf), "Try again in %u seconds",
                    static_cast<unsigned>(rem));
    }
    canvas_.draw_text_centered(kCanvasW / 2, 426, lockbuf, Canvas::TextRole::Secondary, Gray::G0);
  }
}

void App::handle_pin(InputEvent e) {
  // Lockout blocks digit entry — but Back still leaves so the user is never stuck.
  if (now_ms_ < pin_lockout_until_ms_) {
    if (e == InputEvent::Back) {
      // Leaving keeps the lockout armed — re-entering the PIN screen must not reset it.
      parental_pin_for_app_ = false;
      pin_change_ = false;
      pin_entry_.clear();
      pin_enter_empty_slot();
      error_msg_.clear();
      error_until_ms_ = 0;
      nav_.pop();
      after_nav();
    }
    return;
  }
  if (e == InputEvent::Back) {
    if (pin_entry_.empty()) {
      parental_pin_for_app_ = false;
      pin_change_ = false;
      nav_.pop();
      after_nav();
    } else {
      const int prev = static_cast<int>(pin_entry_.size());
      pin_backspace();
      mark_pin_slots_dirty(prev, static_cast<int>(pin_entry_.size()), true);
    }
    return;
  }

  // Preview only — Select commits the digit (avoids slot-jump overwrite).
  // Net deltas from coalesced rotary events go through apply_pin_digit_delta().
  if (e == InputEvent::Up) {
    pin_ignore_select_until_ms_ = 0;
    apply_pin_digit_delta(-1);
    return;
  }
  if (e == InputEvent::Down) {
    pin_ignore_select_until_ms_ = 0;
    apply_pin_digit_delta(1);
    return;
  }
  if (e == InputEvent::Select) {
    const int prev_i = static_cast<int>(pin_entry_.size());
    if (static_cast<int>(pin_entry_.size()) < cfg_.pin_length) {
      if (!pin_commit_working_digit()) return;
    }
    if (static_cast<int>(pin_entry_.size()) >= cfg_.pin_length) {
      if (verify_pin(cfg_, pin_entry_)) {
        pin_fail_count_ = 0;
        pin_spin_count_ = 0;
        pin_ignore_select_until_ms_ = 0;
        if (pin_change_) {
          // Verified the current PIN — now choose the new one.
          pin_entry_.clear();
          pin_pending_.clear();
          pin_enter_empty_slot();
          nav_.replace(ScreenId::OnboardingPinSet);
          after_nav();
          return;
        }
        if (parental_pin_for_app_) {
          parental_session_unlocked_ = true;
          parental_pin_for_app_ = false;
          const HomeApp pending = parental_pending_app_;
          pin_entry_.clear();
          pin_enter_empty_slot();
          nav_.pop();
          launch_home_app(pending);
        } else {
          pin_entry_.clear();
          pin_enter_empty_slot();
          go_home();
        }
      } else {
        error_msg_ = "Incorrect PIN";
        error_until_ms_ = now_ms_ + 2000;
        ++pin_fail_count_;
        pin_entry_.clear();
        pin_spin_count_ = 0;
        pin_ignore_select_until_ms_ = 0;
        pin_enter_empty_slot();
        if (pin_fail_count_ >= 5) {
          pin_lockout_until_ms_ = now_ms_ + 30000;
          pin_lockout_shown_s_ = 0xffffffffu;  // force first countdown tick to paint
          pin_fail_count_ = 0;
        }
        mark_pin_slots_dirty(0, cfg_.pin_length - 1, true);
      }
    } else {
      mark_pin_slots_dirty(prev_i, static_cast<int>(pin_entry_.size()), true);
    }
  }
}


void App::draw_home_clock() {
  int h = 0, m = 0, wd = 0, mo = 0, d = 0;
  clock_.local_hm(h, m, wd, mo, d);
  char tbuf[16];
  if (cfg_.time_format == 24) {
    std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d", h, m);
  } else {
    int h12 = h % 12;
    if (h12 == 0) h12 = 12;
    std::snprintf(tbuf, sizeof(tbuf), "%d:%02d", h12, m);
  }
  constexpr int kClockTop = kContentTop + 40;
  constexpr int kClockH = 176;
  canvas_.fill_rect(kSideMargin, kClockTop, kFocusRowW, kClockH, Gray::G3);
  canvas_.draw_text_centered(kCanvasW / 2, kClockTop + 4, tbuf, Canvas::TextRole::HugeClock, Gray::G0);
  char dbuf[48];
  std::snprintf(dbuf, sizeof(dbuf), "%s, %s %d", kWeekdays[wd % 7], kMonths[mo % 12], d);
  canvas_.draw_text_centered(kCanvasW / 2, kClockTop + 156, dbuf, Canvas::TextRole::Secondary, Gray::G1);
  last_home_clock_minute_ = h * 60 + m;
}

void App::present_home_clock_partial() {
  draw_home_clock();
  constexpr int kClockTop = kContentTop + 40;
  constexpr int kClockH = 176;
  display_.present_region(canvas_, kSideMargin, kClockTop, kFocusRowW, kClockH);
  refresh_.on_applied(RefreshMode::Partial);
}

bool App::screen_has_status_bar() const {
  const ScreenId s = nav_.current();
  if (s == ScreenId::Lock) return false;
  if (s == ScreenId::PassDetail) return false;
  return true;
}

void App::maybe_tick_status_chrome() {
  if (!cfg_.onboarding_complete && nav_.current() == ScreenId::OnboardingWelcome) {
    // Welcome is full-bleed brand; still has status bar in onboarding path.
  }
  // During PIN digit entry, skip status-only region ticks — a status present clears the
  // framebuffer and re-renders the PIN correctly in RAM but only flashes the top strip,
  // which can leave the panel looking like the focused digit "jumped" relative to focus.
  if (is_pin_entry_screen()) return;
  if (!screen_has_status_bar()) {
    // Lock: static art + optional message only — nothing here ticks, so no partial refresh needed.
    return;
  }

  int h = 0, m = 0, wd = 0, mo = 0, d = 0;
  clock_.local_hm(h, m, wd, mo, d);
  const int minute_key = h * 60 + m;
  const int batt = clock_.battery_percent();
  const bool wifi_ok = wifi_.connected() || wifi_.provisioning();
  const bool time_ok = clock_.time_valid();
  const bool needs_sync = clock_.time_needs_network_sync();

  if (last_status_minute_ < 0) {
    last_status_minute_ = minute_key;
    last_status_battery_ = batt;
    last_status_wifi_ = wifi_ok;
    last_status_time_ok_ = time_ok;
    last_status_needs_sync_ = needs_sync;
    return;
  }

  const bool changed = (minute_key != last_status_minute_) || (batt != last_status_battery_) ||
                       (wifi_ok != last_status_wifi_) || (time_ok != last_status_time_ok_) ||
                       (needs_sync != last_status_needs_sync_);
  if (!changed) return;

  last_status_minute_ = minute_key;
  last_status_battery_ = batt;
  last_status_wifi_ = wifi_ok;
  last_status_time_ok_ = time_ok;
  last_status_needs_sync_ = needs_sync;
  mark_status_dirty();
  if (dirty_ && try_present(false)) {
    dirty_ = false;
  }
}

void App::maybe_tick_clock_face() {
  const ScreenId s = nav_.current();
  // render_clock draws the huge time on ClockFace, or when clock_tab_==0 on clock roots.
  const bool on_clock = s == ScreenId::ClockFace || s == ScreenId::ClockAlarms ||
                        s == ScreenId::ClockTimers || s == ScreenId::ClockAlarmEdit ||
                        s == ScreenId::ClockTimerRun;
  if (!on_clock) return;
  if (s != ScreenId::ClockFace && clock_tab_ != 0) return;

  int h = 0, m = 0, wd = 0, mo = 0, d = 0;
  clock_.local_hm(h, m, wd, mo, d);
  const int minute_key = h * 60 + m;
  if (last_home_clock_minute_ < 0) {
    last_home_clock_minute_ = minute_key;
    return;
  }
  if (minute_key == last_home_clock_minute_) return;
  last_home_clock_minute_ = minute_key;
  // Huge clock band (see render_clock y≈280).
  mark_region_dirty(0, 220, kCanvasW, 160);
  if (dirty_ && try_present(false)) {
    dirty_ = false;
  }
}

void App::maybe_tick_home_clock() {
  // Home no longer hosts a large clock — time updates live in the status bar only.
  (void)0;
}

void App::maybe_tick_pin_chrome() {
  if (!is_pin_entry_screen()) {
    pin_chrome_armed_ = false;
    pin_lockout_shown_s_ = 0xffffffffu;
    return;
  }
  const bool err_live = now_ms_ < error_until_ms_ && !error_msg_.empty();
  const bool lock_live = now_ms_ < pin_lockout_until_ms_;
  if (err_live || lock_live) {
    pin_chrome_armed_ = true;
    // Live lockout countdown — refresh chrome once per remaining-second change so
    // "Try again in N seconds" never reads as a frozen 30.
    if (lock_live) {
      const uint32_t rem = (pin_lockout_until_ms_ - now_ms_ + 999u) / 1000u;
      if (rem != pin_lockout_shown_s_) {
        pin_lockout_shown_s_ = rem;
        mark_pin_slots_dirty(0, std::max(0, cfg_.pin_length - 1), true);
        if (dirty_ && try_present(false)) {
          dirty_ = false;
        }
      }
    }
    return;
  }
  // Error / lockout just expired — wipe the chrome band so stale ink doesn't linger.
  if (!pin_chrome_armed_) return;
  pin_chrome_armed_ = false;
  pin_lockout_shown_s_ = 0xffffffffu;
  mark_pin_slots_dirty(0, std::max(0, cfg_.pin_length - 1), true);
  if (dirty_ && try_present(false)) {
    dirty_ = false;
  }
}

void App::maybe_tick_transient_error() {
  if (is_pin_entry_screen()) return;  // PIN chrome path owns expiry on that screen.
  if (error_msg_.empty() || error_until_ms_ == 0) {
    transient_error_armed_ = false;
    return;
  }
  if (now_ms_ < error_until_ms_) {
    transient_error_armed_ = true;
    return;
  }
  if (!transient_error_armed_) return;
  transient_error_armed_ = false;
  error_msg_.clear();
  error_until_ms_ = 0;
  // Footer / toast band — clear residual "offline" / dictation toasts.
  mark_region_dirty(0, kCanvasH - kStatusPillBand, kCanvasW, kStatusPillBand);  // whole toast (2 lines)
  if (dirty_ && try_present(false)) {
    dirty_ = false;
  }
}

}  // namespace pocket