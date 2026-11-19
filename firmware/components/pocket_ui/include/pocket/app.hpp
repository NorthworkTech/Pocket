#pragma once
#include "pocket/canvas.hpp"
#include "pocket/config.hpp"
#include "pocket/input.hpp"
#include "pocket/nav.hpp"
#include "pocket/refresh.hpp"
#include "pocket/screensaver.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pocket {

/** Lock/sleep face rotates through the generative screensaver collection (screensaver.hpp). */
inline constexpr int kLockMotifCount = kScreensaverCount;

struct PlatformClock {
  virtual ~PlatformClock() = default;
  virtual uint32_t now_ms() = 0;
  virtual void local_hm(int& hour, int& minute, int& weekday, int& month, int& day) = 0;
  /** True when a wall time can be shown (trusted or approximate offline restore). */
  virtual bool time_valid() const { return true; }
  /** True when time is network/RTC-trusted — not an Approximate post-power-loss restore. */
  virtual bool time_trusted() const { return time_valid(); }
  /** Hint UX to ask for Wi‑Fi sync (unsynced or approximate). */
  virtual bool time_needs_network_sync() const { return false; }
  /** Block the UI task briefly (network retries). Host: no-op so tests stay fast. */
  virtual void sleep_ms(uint32_t /*ms*/) {}
  /** Battery 0–100 from PMIC; USB-only boards may report 100. */
  virtual int battery_percent() { return 100; }
};

struct PlatformWifi {
  virtual ~PlatformWifi() = default;
  virtual std::vector<std::string> scan() = 0;
  virtual bool connect(const std::string& ssid, const std::string& password) = 0;
  virtual bool connected() const = 0;
  /** SoftAP + HTTP portal so the phone can send Wi‑Fi credentials (no on-device typing). */
  virtual bool start_provision(const std::string& preferred_ssid, std::string* ap_ssid_out,
                               std::string* ap_pass_out) = 0;
  /**
   * Build SoftAP SSID/password without enabling AP radio. Lets the SoftAP screen
   * paint before RF (APSTA) so e-ink + TX do not race Power-line glitches / brownout.
   */
  virtual bool peek_provision_identity(std::string* ap_ssid_out, std::string* ap_pass_out) {
    if (ap_ssid_out) *ap_ssid_out = provision_ap_ssid();
    if (ap_pass_out) *ap_pass_out = provision_ap_password();
    return !(ap_ssid_out && ap_ssid_out->empty());
  }
  virtual void stop_provision() = 0;
  virtual bool take_provision_credentials(std::string* ssid, std::string* password) = 0;
  virtual std::string provision_ap_ssid() const = 0;
  virtual std::string provision_ap_password() const = 0;
  /** Plain-language reason the last join failed (wrong password, network not found…). */
  virtual std::string last_error() const { return {}; }
  /** SoftAP portal currently advertising (phone can join). */
  virtual bool provisioning() const { return false; }
};

struct PlatformCloud {
  virtual ~PlatformCloud() = default;
  /** Mint pairing session; returns plaintext code for QR display. */
  virtual std::string create_pair_session(const std::string& device_id) = 0;
  virtual std::string pair_status(const std::string& code) = 0;  // pending|claimed|expired
  virtual void refresh_entitlement(DeviceConfig& cfg) = 0;
  virtual std::string stt_transcribe(const std::vector<uint8_t>& pcm) = 0;
  /** List music library metadata JSON (id/title/filename/size). Empty on failure. */
  virtual std::string music_list_json(const std::string& /*device_id*/, bool /*sd_present*/ = false) {
    return "[]";
  }
  /** Download track audio bytes by id. Returns empty on failure. */
  virtual std::vector<uint8_t> music_download(const std::string& /*device_id*/,
                                              const std::string& /*track_id*/) {
    return {};
  }
  /**
   * Download a track ("music") or book text ("book") straight to `path`, never holding
   * the whole file in RAM. Writes path.part and renames on success, so an interrupted
   * download can't masquerade as complete. Fails if the body exceeds `max_bytes` (>0).
   * Default: buffer through music_download/book_download (host builds and tests).
   */
  virtual bool download_to_file(const char* kind, const std::string& device_id, const std::string& id,
                                const std::string& path, int64_t max_bytes, int64_t* bytes_out);
  /** List Reading library metadata JSON (id/title/author/format/filename/size). Empty on failure. */
  virtual std::string books_list_json(const std::string& /*device_id*/) { return "[]"; }
  /** Download the normalized plain-text body of a book by id. Empty on failure. */
  virtual std::vector<uint8_t> book_download(const std::string& /*device_id*/,
                                             const std::string& /*book_id*/) {
    return {};
  }
  /** Latest firmware metadata JSON: build_id, version, url, size. Empty on failure. */
  virtual std::string firmware_latest_json() { return {}; }
  /**
   * Heartbeat + entitlement + optional pending Wi‑Fi / parental JSON.
   * Returns raw JSON body; empty on failure.
   */
  virtual std::string device_attest_json(const std::string& /*device_id*/) { return {}; }
  /** Tell Pocket Cloud whether a microSD is in (raises Companion upload limits). -1 = unknown. */
  virtual void set_sd_present(int /*sd*/) {}
  /** Upcoming calendar events (ICS feeds linked in Companion): {"events":[…]}. Empty on failure. */
  virtual std::string calendar_json(const std::string& /*device_id*/) { return {}; }
  /** Save a settings/alarms/reading backup to Pocket Cloud. */
  virtual bool post_backup(const std::string& /*device_id*/, const std::string& /*body*/) { return false; }
  /** Latest backup from any of this account's Pockets: {"backup":{…}}. Empty if none/failure. */
  virtual std::string backup_latest_json(const std::string& /*device_id*/) { return {}; }
  /** Report reading progress for Companion ("Walden · 42%"). Fire-and-forget. */
  virtual bool post_reading_progress(const std::string& /*device_id*/, const std::string& /*book_id*/,
                                     int /*page*/, int /*pages*/, int /*percent*/) {
    return false;
  }
  /** Plain-language reason the last request failed (no internet, server error…). */
  virtual std::string last_error() const { return {}; }
  /** POST /v1/device/sync — push local note/list changes, get the merged set back. */
  virtual std::string device_sync_json(const std::string& /*device_id*/, const std::string& /*body*/,
                                       int* status) {
    if (status) *status = 0;
    return {};
  }
  /** Best-effort mirror of a rotary-set volume/brightness change up to Companion. Fire-and-forget. */
  virtual bool push_device_settings(const std::string& /*device_id*/, int /*volume_percent*/,
                                    int /*brightness_percent*/) {
    return false;
  }
  /** Plain GET (no device auth) for public APIs such as Open-Meteo. Empty on failure. */
  virtual std::string http_get_text(const std::string& /*url*/) { return {}; }
};

struct FirmwareUpdateInfo {
  std::string build_id;
  std::string version;
  std::string url;
  int size_bytes = 0;
  /** Human name from release.json, e.g. "PocketOS 2.1" (empty on older manifests). */
  std::string version_name;
  std::vector<std::string> notes;
};

struct PlatformOta {
  virtual ~PlatformOta() = default;
  /** Download `url` into the inactive OTA slot and set boot partition. Blocking.
   * `progress` (optional) receives 0–100 as the image downloads. */
  virtual bool apply_https_ota(const std::string& url, std::string* status_out,
                               const std::function<void(int)>& progress = {}) {
    (void)progress;
    if (status_out) *status_out = "OTA not available.";
    return false;
  }
};

struct PlatformDisplay {
  virtual ~PlatformDisplay() = default;
  virtual void present(const Canvas& canvas, RefreshMode mode) = 0;
  /** Partial refresh of a logical rectangle (clock tick). Falls back to full-canvas partial. */
  virtual void present_region(const Canvas& canvas, int /*x*/, int /*y*/, int /*w*/, int /*h*/) {
    present(canvas, RefreshMode::Partial);
  }
  /**
   * True while a panel refresh is in flight. When busy, App keeps dirty state and continues
   * applying rotary deltas — never drop PIN/nav ticks waiting on a partial.
   */
  virtual bool busy() { return false; }
  /** Block (up to `max_ms`) until an in-flight refresh finishes. Host: no-op. */
  virtual void wait_idle(uint32_t /*max_ms*/) {}
  /** 0–100: how aggressively midtone gray rounds to black vs white on a monochrome panel.
   * Higher reads lighter/brighter, lower reads darker/higher-contrast. No-op on displays that
   * don't expose this (e.g. host sim). */
  virtual void set_brightness(int /*percent*/) {}
};

enum class SdContentKind : uint8_t {
  Absent = 0,
  Empty,
  Media,
  FirmwareRisk,
  Unknown,
};

struct PlatformStorage {
  virtual ~PlatformStorage() = default;
  virtual bool probe() { return false; }
  virtual bool present() { return false; }
  virtual SdContentKind classify() { return SdContentKind::Absent; }
  virtual bool erase_card() { return false; }
  virtual void unmount() {}
  /** Prefer SD when mounted + space; else LittleFS internal. Empty if neither usable. */
  virtual std::string music_root() { return {}; }
  virtual bool music_ensure_root() { return false; }
  virtual uint64_t free_bytes(const std::string& /*root*/) { return 0; }
  /** True when music_root is on the microSD mount (larger tracks OK). */
  virtual bool music_on_sd() { return false; }
  /** Prefer SD when mounted; else LittleFS internal. Empty if neither usable. */
  virtual std::string book_root() { return {}; }
  virtual bool book_ensure_root() { return false; }
  /** Local user-data root for Notes/Lists/Passes/weather JSON.
   * Prefer LittleFS; may fall back to SD (`/sdcard/pocket`). Empty if unavailable. */
  virtual std::string data_root() { return {}; }
  virtual bool data_ensure_root() { return false; }
};

enum class SoundId : uint8_t { Click = 0, Welcome, Success, Attention };

/** Result of a PTT-bounded mic capture window (Settings → Sound → Mic test, onboarding Voice step). */
struct MicCaptureResult {
  /** True when the board has a working mic path and captured at least one chunk. */
  bool ok = false;
  /** 0–100 loudness estimate (RMS) across the whole hold — real signal, not simulated. */
  int level_percent = 0;
  /** Raw 16-bit PCM mono samples captured (16 kHz), for optional cloud dictation. */
  std::vector<uint8_t> pcm;
};

struct PlatformAudio {
  virtual ~PlatformAudio() = default;
  virtual void play(SoundId /*id*/) {}
  /** Play a PCM/WAV file from local path (best-effort; may block). */
  virtual bool play_file(const std::string& /*path*/) { return false; }
  virtual void stop() {}
  /** True while play_file() audio is still playing (playback runs in the background). */
  virtual bool is_playing() { return false; }
  /** DAC output level, 0–100. Applied immediately when the codec is ready. */
  virtual void set_volume(int /*percent*/) {}
  /** True when this board exposes a real mic capture path (onboard PDM/I2S mic + codec ADC). */
  virtual bool mic_supported() { return false; }
  /** Begin a capture window (call on PTT down). Safe no-op if unsupported. */
  virtual void start_capture() {}
  /** Non-blocking: pull whatever PCM has accumulated since start/last poll (call from tick). */
  virtual void poll_capture() {}
  /** End the capture window (call on PTT up) and return the accumulated result. */
  virtual MicCaptureResult stop_capture() { return {}; }
};

struct MusicTrack {
  std::string id;
  std::string title;
  std::string filename;
  std::string local_path;
  int size_bytes = 0;
};

/** eBook metadata + device-local copy. Device always reads plain text; `format` records the
 * original upload so Companion/UI can show it (EPUB text is normalized server-side). */
struct Book {
  std::string id;
  std::string title;
  std::string author;
  std::string format;  // "epub" | "txt"
  std::string filename;
  std::string local_path;
  int size_bytes = 0;
};

/** Optional: stable device UUID from MAC (ESP). Empty → App generates placeholder. */
struct PlatformIdentity {
  virtual ~PlatformIdentity() = default;
  virtual std::string device_uuid() { return {}; }
};

struct Note {
  std::string id;
  std::string title;
  std::string body;
  int64_t created_at = 0;
  int64_t updated_at = 0;
  /** Relative filename under notes/pcm/ — cloud STT when back online. Empty = none. */
  std::string pending_pcm;
};

struct ListItem {
  std::string id;
  std::string text;
  bool checked = false;
  int64_t updated_at = 0;
  /** Relative filename under notes/pcm/ — cloud STT when back online. Empty = none. */
  std::string pending_pcm;
};

struct TodoList {
  std::string id;
  std::string title;
  std::vector<ListItem> items;
  int64_t updated_at = 0;
  /** Pocket Family: shared into the family (owner = who shared it). From sync. */
  bool shared = false;
  std::string owner;
};

struct Alarm {
  std::string id;
  int minutes_of_day = 0;
  bool enabled = true;
  std::string label;
};

struct Pass {
  std::string id;
  std::string title;
  std::string type;
  std::string payload;
};

struct WeatherDay {
  std::string date;
  int hi = 0;
  int lo = 0;
  std::string condition;
};

struct WeatherCache {
  std::string city;
  uint8_t units = 0;
  int64_t fetched_at = 0;
  std::vector<WeatherDay> days;
  int today_temp = 0;
  std::string today_condition;
  /** Local minutes-of-day of the last fetch (-1 = unknown) for "Updated 9:41". */
  int fetched_hm = -1;
};

struct AppData {
  std::vector<Note> notes;
  std::vector<TodoList> lists;
  std::vector<Alarm> alarms;
  std::vector<Pass> passes;
  WeatherCache weather;
};

/** Focusable row helper for list UIs. */
struct FocusModel {
  int index = 0;
  int count = 0;
  void move(int delta) {
    if (count <= 0) {
      index = 0;
      return;
    }
    index = (index + delta) % count;
    if (index < 0) index += count;
  }
};

class App {
 public:
  App(ConfigStore& store, PlatformClock& clock, PlatformWifi& wifi, PlatformCloud& cloud,
      PlatformDisplay& display, PlatformStorage* storage = nullptr, PlatformAudio* audio = nullptr,
      PlatformIdentity* identity = nullptr, PlatformOta* ota = nullptr);

  void boot();
  void set_build_id(std::string_view id);
  void tick(uint32_t now_ms);
  void handle(InputEvent e);
  /** True while the user is spinning/committing PIN digits (lock or onboarding). */
  bool is_pin_entry_screen() const;
  /** True on the Home app grid (rotary focus coalescing). */
  bool is_home_screen() const;
  /** Apply a net rotary delta to the working PIN digit (Down=+1) and dirty once. */
  void apply_pin_digit_delta(int delta);
  /** Apply a net rotary delta to Home focus and dirty once (region of changed tiles). */
  void apply_home_focus_delta(int delta);
  /**
   * Apply a net Up/Down rotary burst once: PIN digit, Home tiles, or in-page focus.
   * Defers present so one physical burst → one refresh (caller should flush_dirty).
   */
  void apply_coalesced_rotary(int delta);
  /**
   * Present the canvas if a dirty mark is pending (used after coalesced PIN/Home spins).
   * No-ops while display_.busy() — dirty stays set so a later flush paints the latest state.
   */
  void flush_dirty();

  /** Committed PIN digits so far (length == focus index). */
  const std::string& pin_entry() const { return pin_entry_; }
  /** Working spin glyph for the focused (next) slot — always matches what draw paints there. */
  char pin_working_digit() const { return pin_digit_working_; }
  /** Focused slot index (== pin_entry().size()); never skips ahead of committed digits. */
  int pin_focus_index() const { return static_cast<int>(pin_entry_.size()); }
  /** Remaining lockout seconds (ceil); 0 when not locked out. Host/tests. */
  uint32_t pin_lockout_remaining_s() const {
    if (now_ms_ >= pin_lockout_until_ms_) return 0;
    return (pin_lockout_until_ms_ - now_ms_ + 999u) / 1000u;
  }
  /** True while Incorrect-PIN / lockout chrome is armed. Host/tests. */
  bool pin_chrome_live() const { return pin_chrome_armed_; }
  /**
   * Logical value shown for slot `i`: committed digit, working digit if focused,
   * or `'0'` when the slot is still unset (empty, not focused).
   */
  char pin_slot_value(int i) const;

  ScreenId screen() const { return nav_.current(); }
  const DeviceConfig& config() const { return cfg_; }
  /** Which of kLockMotifCount calm designs the lock/sleep face is currently showing. */
  int lock_motif_index() const { return lock_motif_index_; }
  /** Home grid focus index (host/tests). */
  int home_focus_index() const { return focus_.index; }
  /**
   * After SoftAP radio starts, main loop must snapshot button levels and drop
   * Power/Home/Back that RF edge-glitched into the queue. Returns true once.
   */
  bool consume_input_resync() {
    if (!input_resync_pending_) return false;
    input_resync_pending_ = false;
    return true;
  }
  const Canvas& canvas() const { return canvas_; }
  bool needs_redraw() const { return dirty_; }

  /** STA just associated — refresh clock sync + Cloud heartbeat promptly. */
  void notify_wifi_connected();

  /** Force render for host/tests. Keeps dirty if the panel is mid-refresh. */
  void redraw(bool full);

#ifdef POCKET_HOST
  /** Host gallery / QA only: jump straight to `s` with Home underneath. */
  void host_goto(ScreenId s, int tab = 0);
  AppData& host_data() { return data_; }
  std::vector<MusicTrack>& host_tracks() { return music_tracks_; }
  std::vector<Book>& host_books() { return books_; }
  /** Start a countdown (host QA / tests). */
  void host_start_timer(uint32_t ms) {
    timer_total_ms_ = ms;
    timer_end_ms_ = now_ms_ + ms;
    timer_running_ = true;
    timer_paused_ = false;
  }
  bool ringing() const { return nav_.current() == ScreenId::ClockRinging; }
  const std::string& status_message() const { return status_msg_; }
  bool showing_update_welcome() const { return show_update_welcome_; }
  void host_set_update(const FirmwareUpdateInfo& info) { update_info_ = info; }
  void host_set_installing(bool on, int pct) { ota_installing_ = on; ota_progress_ = pct; }
  void host_set_update_welcome(bool on) { show_update_welcome_ = on; }
  void host_show_status(std::string_view m) { show_status(m); }
  void host_set_tips_page(int p) { tips_page_ = p; }
  std::string host_weather_label() const { return weather_updated_label(); }
  std::string host_lock_info() const { return lock_info_line(); }
  DeviceConfig& mut_config() { return cfg_; }
  bool host_list_command(const std::string& t) { return apply_list_command(t); }
  void host_page_left(bool fwd) { reading_note_page_left(fwd); }
  int host_minutes_left() const { return reading_minutes_left(); }
  float host_wpm() const { return reading_wpm_; }
  void host_fetch_calendar() { maybe_fetch_calendar(true); }
  bool host_next_event(std::string* l, std::string* m) const { return next_calendar_event(l, m); }
  bool host_saver() const { return battery_saver_; }
  void host_backup() { maybe_backup(true); }
  bool host_apply_backup(const std::string& j) { return apply_backup(j); }
  std::string host_backup_payload() const { return backup_payload(); }
  bool host_quiet_now() const { return quiet_now(); }
  void host_attest() { last_attest_ms_ = 0; maybe_cloud_attest(); }
  bool host_cloud_sync() { return cloud_sync(false); }
  void host_reading_sync() { reading_sync_from_cloud(); }
  void host_reading_load() { reading_load_local(); }
  void host_reading_open(int i) { reading_open_book(i); }
  const std::string& host_reading_text() const { return reading_text_; }
  const std::string& host_reading_status() const { return reading_status_; }
  int host_note_pages() const { return note_pages_; }
  /** Open an in-memory book page for gallery/QA. */
  void host_open_book(const std::string& text, int page = 0) {
    book_index_ = 0;
    reading_text_ = text;
    reading_paginate();
    reading_page_ = page;
    focus_.index = 1;
  }
#endif

 private:
  enum class DirtyKind : uint8_t { FullCanvas, ContentBand, StatusBar, Region };

  void render();
  void draw_status_bar();
  void go_home();
  void go_lock();
  /** Abort an in-flight push-to-talk capture (navigation away, lock). */
  void cancel_capture();
  /** SoftAP / Add-network screens — idle lock must not interrupt phone join. */
  bool wifi_add_flow_active() const;
  /** True during SoftAP RF settle window (Power/Back/Home suppressed). */
  bool wifi_rf_guard_active() const;
  /** Finish deferred SoftAP radio once the panel is idle (paint-before-RF). */
  void maybe_start_pending_softap_radio();
  /** Arm RF guard + request GPIO resync after SoftAP mode changes. */
  void arm_wifi_rf_guard();
  void after_nav(bool full_refresh);
  /** Navigate then refresh using Spec §6 (full only for major enters / QR / PIN). */
  void after_nav();
  /**
   * Present when the panel is idle. Returns false if busy (caller must keep dirty_).
   * Critical after v90 async e-ink: never clear dirty when present was skipped.
   */
  bool try_present(bool full);
  /**
   * Tell the user what's happening before a blocking call (sync, transcribe, connect):
   * paints a status pill at the bottom right now. Cleared on the next flush.
   */
  void show_status(std::string_view msg);
  // ---- Companion sync (sync.cpp) ----
  bool cloud_sync_possible() const;
  /** Push local note/list changes and adopt the merged result. Blocking; true on success. */
  bool cloud_sync(bool show_progress);
  void maybe_cloud_sync();
  bool sd_present() { return storage_ && storage_->present(); }
  // ---- 2.2 features ----
  /** Inside configured quiet hours (clicks/chimes muted; alarms still ring). */
  bool quiet_now() const;
  /** Sounds that must play even in quiet hours: alarms, timers, Find my Pocket. */
  void play_sound_urgent(SoundId id);
  void ring_find_my_pocket();
  /** "add milk to groceries" → item on that list (list created if needed). True if handled. */
  bool apply_list_command(const std::string& transcript);
  // ---- 2.3 (features_23.cpp) ----
  struct CalEvent {
    std::string title, location;
    int64_t start = 0, end = 0;  // unix seconds
    bool all_day = false;
  };
  std::vector<CalEvent> calendar_;
  uint32_t calendar_fetch_ms_ = 0;
  void calendar_load_cache();
  void maybe_fetch_calendar(bool force);
  /** "10:30 AM · Dentist" + "Tomorrow · Main St" for the next event, or empty. */
  bool next_calendar_event(std::string* line, std::string* meta) const;
  bool battery_saver_ = false;
  void maybe_battery_saver();
  bool lock_night_shown_ = false;
  void render_night_face();
  void maybe_backup(bool force);
  bool backup_requested_ = false;
  int64_t last_backup_unix_ = 0;
  std::string backup_payload() const;
  bool apply_backup(const std::string& backup_json);
  std::string restore_offer_json_;
  uint32_t last_saver_check_ms_ = 0;
  uint32_t last_backup_attempt_ms_ = 0;
  void render_restore();
  void handle_restore(InputEvent e);
  /** After linking during setup: fetch the newest backup; true if one is on offer. */
  bool offer_restore_if_any();
  std::string restore_offer_label_;
  // ---- Reading stats (reading_stats.cpp) ----
  std::string reading_day_key(int days_ago) const;
  void reading_stats_load();
  void reading_stats_save();
  /** Call when leaving the current page (`forward` = a Next turn). */
  void reading_note_page_left(bool forward);
  int reading_minutes_left() const;
  std::string reading_time_left_label() const;
  std::string reading_summary_line();
  bool reading_stats_loaded_ = false;
  float reading_wpm_ = 0;
  int reading_wpm_samples_ = 0;
  int reading_turns_since_save_ = 0;
  uint32_t reading_page_shown_ms_ = 0;
  std::map<std::string, int> reading_day_seconds_;
  void render_today();
  void handle_today(InputEvent e);
  /** Small static line under the lock wordmark: date · next alarm · weather. */
  std::string lock_info_line() const;
  uint32_t last_lock_info_ms_ = 0;
  bool lock_recording_ = false;
  std::string lock_flash_;
  uint32_t lock_flash_until_ms_ = 0;
  std::string lock_info_shown_;
  /** Reading progress waiting to go to Companion. */
  bool reading_progress_dirty_ = false;
  uint32_t last_reading_push_ms_ = 0;
  void maybe_push_reading_progress(bool force);
  int wifi_detail_index_ = -1;
  int settings_root_scroll_ = 0;
  std::string new_local_id(const char* prefix);
  uint32_t last_sync_attempt_ms_ = 0;
  uint32_t last_sync_ok_ms_ = 0;
  uint32_t id_counter_ = 0;
  int last_reported_sd_ = -1;
  /** Why the last pairing-code request failed (shown on the Link screen). */
  std::string pair_error_;
  /** Setup was interrupted by a restart after Wi-Fi joined: rejoin, then go to Link. */
  bool setup_rejoin_pending_ = false;
  /** SD erase in progress — the SD screens show "Reformatting…" instead of looking frozen. */
  bool sd_formatting_ = false;
  /** "Last updated today at 9:41 AM" / "…yesterday at…" / "…Monday at…" / "…Nov 12". */
  std::string weather_updated_label() const;
  bool reformat_sd_with_feedback();
  void maybe_setup_rejoin();
  std::string sync_status_;
  /** Full refresh after a pause when partials have left ghosting (see RefreshPolicy). */
  void maybe_idle_clean();
  /** Music, recording, a running timer, an install… — don't auto-lock under them. */
  bool keeps_awake() const;
  /** Auto-lock delay for the current screen (reading screens get at least 10 min). */
  uint32_t idle_lock_after_ms() const;
  static constexpr uint32_t kIdleCleanAfterMs = 2500;
  uint32_t last_present_ms_ = 0;
  bool want_full_ = false;
  bool want_deep_ = false;
  std::string status_msg_;
  void mark_content_dirty();
  void mark_status_dirty();
  /** Tight e-ink region update (PIN digits, focus rows, etc.). */
  void mark_region_dirty(int x, int y, int w, int h);
  void mark_pin_dirty();
  void mark_pin_slots_dirty(int slot_a, int slot_b, bool with_chrome);
  /** Dirty the union of previous/next Home tiles. */
  void mark_home_focus_dirty(int prev_index, int next_index);
  /** Dirty the union of two Settings-style focus rows. */
  void mark_list_focus_dirty(int list_top, int prev, int next);
  void settings_move_focus(int delta);
  int settings_focus_list_top() const;
  void present_canvas(bool full);
  void play_sound(SoundId id);
  bool mint_pair_session();
  void begin_softap_link();
  /** Resume mid-setup from saved DeviceConfig fields (avoid Welcome loop). */
  ScreenId resume_onboarding_screen() const;
  void ensure_softap_credentials_shown();
  /** Draw PIN slots; mask_completed hides entered digits as dots (unlock). */
  void draw_pin_entry(bool mask_completed, int band_top);
  void pin_band_geometry(int& x, int& y, int& w, int& h) const;
  /** Enter a fresh empty slot: focus = entry size, working digit = '0', spin latch clear. */
  void pin_enter_empty_slot();
  /** Commit working digit (if room); returns true if a digit was appended. */
  bool pin_commit_working_digit();
  /** Erase last committed digit and refocus that slot at '0'. */
  void pin_backspace();

  // Screen handlers
  void render_lock();
  void handle_lock(InputEvent e);
  void render_pin();
  void handle_pin(InputEvent e);
  void render_onboarding();
  void handle_onboarding(InputEvent e);
  void render_home();
  void handle_home(InputEvent e);
  void draw_home_clock();
  void draw_lock_motif();
  /** Small geometric mark to the left of the "Pocket" wordmark on the lock face. */
  void draw_pocket_mark(int right_x, int cy);
  void maybe_tick_home_clock();
  void present_home_clock_partial();
  /** Status bar time / battery / wifi — region partial, not full screen. */
  void maybe_tick_status_chrome();
  /** Clock app huge time — region partial on minute change. */
  void maybe_tick_clock_face();
  /** Clear PIN error/lockout chrome when the timer expires (avoids stale ink). */
  void maybe_tick_pin_chrome();
  /** Clear footer error toasts (Notes/Weather/etc.) when error_until_ms_ elapses. */
  void maybe_tick_transient_error();
  bool screen_has_status_bar() const;
  void render_notes();
  void handle_notes(InputEvent e);
  void load_local_notes();
  /** Persist notes to LittleFS/SD. Returns false and sets a short error toast on failure. */
  bool save_local_notes();
  void load_local_lists();
  bool save_local_lists();
  /** Save raw PCM for offline dictation; returns relative name under notes/pcm/. */
  bool save_pending_pcm(const std::vector<uint8_t>& pcm, std::string* out_name);
  void delete_pending_pcm(const std::string& name);
  /** Apply a finished transcript to the current Notes/Lists target (online path). */
  void apply_dictation_transcript(const std::string& t);
  /** Queue mic PCM while offline; honest placeholder until cloud STT runs. */
  /** Save a voice clip for later transcription. `open_note` = false keeps the current screen
   * (lock-screen capture must never navigate past the PIN). */
  bool queue_offline_dictation(const std::vector<uint8_t>& pcm, bool open_note = true);
  /** When online: transcribe any pending voice clips and replace placeholders. */
  void flush_pending_dictation();
  bool notes_have_pending_dictation() const;
  void load_local_passes();
  void save_local_passes();
  void load_weather_cache();
  void save_weather_cache();
  void weather_refresh();
  /** First Weather open: pick the city for the device's time zone so it works out of the box. */
  void weather_ensure_city();
  /** Convert a cached temperature to the currently selected unit. */
  int weather_display_temp(int v) const;
  bool weather_refresh_pending_ = false;
  /** Lowest battery threshold already warned about (100 = none yet). */
  int battery_warned_ = 100;
  uint32_t last_battery_check_ms_ = 0;
  bool weather_geocode_city();
  bool weather_fetch_forecast();
  void music_load_local();
  void reading_load_local();
  void render_ledger();
  void handle_ledger(InputEvent e);
  void render_clock();
  void handle_clock(InputEvent e);
  void load_alarms();
  void save_alarms();
  /** Once per minute: fire due alarms / snooze; drive the active timer and ringing. */
  void maybe_tick_clock_events();
  void start_ringing(bool from_timer);
  void stop_ringing(bool snooze);
  void open_alarm_editor(int index);
  /** Remaining timer ms (0 when idle or done). */
  uint32_t timer_remaining_ms() const;
  void render_pass();
  void handle_pass(InputEvent e);
  void render_weather();
  void handle_weather(InputEvent e);
  void render_music();
  void handle_music(InputEvent e);
  void music_sync_from_cloud();
  void render_reading();
  void handle_reading(InputEvent e);
  void reading_sync_from_cloud();
  /** Apply 72h offline entitlement grace when Cloud is unreachable. */
  void apply_cloud_offline_grace();
  /** Load a book's text, compute e-ink page boundaries, resume last page if it matches. */
  void reading_open_book(int index);
  /** Recompute `reading_pages_` word-wrap boundaries for `reading_text_` at content width. */
  void reading_paginate();
  void render_settings();
  void handle_settings(InputEvent e);
  void render_sd_gate();
  void handle_sd_gate(InputEvent e);
  void enter_sd_gate_from_hotplug();
  void leave_sd_gate();
  void maybe_poll_sd_hotplug();
  void refresh_sd_kind();
  void begin_firmware_update();
  void install_firmware_update();
  void render_update();
  void handle_update(InputEvent e);
  bool parse_firmware_latest(const std::string& json, FirmwareUpdateInfo* out);
  /** Persist SSID+pass and attempt STA connect (blocking). */
  bool connect_and_remember(const std::string& ssid, const std::string& password);
  /** After boot / when idle: try preferred then other known networks. */
  void maybe_wifi_auto_reconnect();
  /** Start SoftAP to add another known network (keeps existing list). */
  void begin_add_wifi_network();
  /** Poll Cloud when online: last_seen, entitlement, pending Wi‑Fi, parental. */
  void maybe_cloud_attest();
  bool parental_requires_pin(HomeApp app) const;
  void launch_home_app(HomeApp app);

  ConfigStore& store_;
  PlatformClock& clock_;
  PlatformWifi& wifi_;
  PlatformCloud& cloud_;
  PlatformDisplay& display_;
  PlatformStorage* storage_ = nullptr;
  PlatformAudio* audio_ = nullptr;
  PlatformIdentity* identity_ = nullptr;
  PlatformOta* ota_ = nullptr;

  DeviceConfig cfg_{};
  AppData data_{};
  NavStack nav_{};
  Canvas canvas_{};
  RefreshPolicy refresh_{};
  InputMapper input_{};  // unused when events injected externally
  bool dirty_ = true;
  DirtyKind dirty_kind_ = DirtyKind::FullCanvas;
  int dirty_rx_ = 0;
  int dirty_ry_ = 0;
  int dirty_rw_ = 0;
  int dirty_rh_ = 0;

  // UI transient state
  FocusModel focus_{};
  std::string pin_entry_;
  std::string pin_pending_;
  char pin_digit_working_ = '0';
  /** Last `band_top` passed to draw_pin_entry (lock vs onboarding). */
  int pin_draw_band_top_ = 200;
  /** True after a rotary spin on the current focus slot. */
  bool pin_spun_since_focus_ = false;
  /**
   * After a spun digit commit, ignore Select until this timestamp (ms).
   * Time-windowed (not one-shot): mechanical bounce is blocked, but an intentional
   * press for the next digit (including default '0') still works after ~120 ms.
   */
  uint32_t pin_ignore_select_until_ms_ = 0;
  /** Counts digit spins since PIN enter — every few spins use a wider refresh to clear ghosts. */
  int pin_spin_count_ = 0;
  /** Counts Home focus moves — every few moves use content-band to clear tile ghosts. */
  int home_focus_spin_count_ = 0;
  /** Counts in-page list focus moves — same ghost cadence as Home. */
  int list_focus_spin_count_ = 0;
  /** When true, handle() marks dirty but does not present (coalesced rotary). */
  bool defer_handle_present_ = false;
  int pin_fail_count_ = 0;
  uint32_t pin_lockout_until_ms_ = 0;
  /** Last painted lockout remaining-seconds (UINT32_MAX = none); drives 1 Hz chrome ticks. */
  uint32_t pin_lockout_shown_s_ = 0xffffffffu;
  uint32_t error_until_ms_ = 0;
  /** Armed while PIN error/lockout chrome is painted — cleared on expiry tick. */
  bool pin_chrome_armed_ = false;
  /** Armed while a non-PIN error toast is painted — cleared on expiry tick. */
  bool transient_error_armed_ = false;
  std::string error_msg_;
  std::string wifi_password_;
  std::vector<std::string> wifi_networks_;
  std::string wifi_ap_ssid_;
  std::string wifi_ap_pass_;
  /** SoftAP screen is connecting to home Wi‑Fi (no Connecting screen remount). */
  bool wifi_sta_connecting_ = false;
  uint32_t last_wifi_prov_poll_ms_ = 0;
  uint32_t last_wifi_reconnect_ms_ = 0;
  bool wifi_boot_reconnect_done_ = false;
  std::string pair_code_;
  uint32_t pair_expires_ms_ = 0;
  uint32_t last_pair_poll_ms_ = 0;
  std::string pair_status_ = "pending";
  int charset_index_ = 0;
  int caret_ = 0;
  std::string picker_buf_;
  int notes_tab_ = 0;  // 0 Notes 1 Lists
  int clock_tab_ = 0;
  // Alarms editor
  int alarm_edit_index_ = -1;  // -1 = new alarm
  Alarm alarm_draft_{};
  /** 0 = browsing rows, 1 = adjusting hour, 2 = adjusting minute. */
  int alarm_adjust_ = 0;
  // Timer
  uint32_t timer_total_ms_ = 0;
  uint32_t timer_end_ms_ = 0;
  uint32_t timer_paused_left_ms_ = 0;
  bool timer_running_ = false;
  bool timer_paused_ = false;
  int timer_shown_s_ = -1;
  // Alerts
  bool ring_from_timer_ = false;
  uint32_t ring_next_beep_ms_ = 0;
  uint32_t ring_until_ms_ = 0;
  int last_alarm_minute_key_ = -1;
  /** One-shot snooze alarm (minutes of day), -1 = none. */
  int snooze_minutes_ = -1;
  int note_index_ = 0;
  /** Page of a long note body on NotesDetail (reset when a note opens). */
  int note_page_ = 0;
  int note_pages_ = 1;
  int music_index_ = 0;
  std::vector<MusicTrack> music_tracks_;
  bool music_playing_ = false;
  std::string music_status_;
  int book_index_ = 0;
  std::vector<Book> books_;
  std::string reading_status_;
  std::string reading_text_;
  std::vector<size_t> reading_pages_;
  int reading_page_ = 0;
  uint32_t last_input_ms_ = 0;
  uint32_t now_ms_ = 0;
  bool ptt_active_ = false;
  std::string mic_result_;
  /** Last mic-test loudness 0–100, -1 = not yet tested this session. */
  int mic_last_level_ = -1;
  bool mic_last_ok_ = false;
  /** Which screen armed the current PTT hold, so tick() knows whether to poll capture. */
  bool mic_capturing_ = false;
  int onboarding_tz_index_ = 0;
  int last_home_clock_minute_ = -1;
  int last_status_minute_ = -1;
  int last_status_battery_ = -1;
  bool last_status_wifi_ = false;
  bool last_status_time_ok_ = false;
  /** Tracks Approximate↔Trusted so status chrome refreshes on quality change. */
  bool last_status_needs_sync_ = false;
  /** Which calm e-ink motif to draw on the lock/sleep face; advances on each lock. */
  int lock_motif_index_ = 0;
  /** Varies each lock's print of the current piece (same piece, fresh composition). */
  uint32_t lock_seed_ = 1;
  SdContentKind sd_kind_ = SdContentKind::Absent;
  bool sd_waiting_eject_ = false;
  uint32_t last_sd_poll_ms_ = 0;
  /** Edge-detect hot-insert after onboarding (true while card still seated after gate). */
  bool sd_was_present_ = false;
  bool sd_gate_active_ = false;
  ScreenId sd_return_screen_ = ScreenId::Home;
  std::string ota_status_;
  /** SettingsUpdateProgress shows checking (false) or installing (true). */
  bool ota_installing_ = false;
  FirmwareUpdateInfo update_info_{};
  /** First boot of a new build: lock face shows "Welcome to PocketOS x" until pressed. */
  bool show_update_welcome_ = false;
  /** Download progress 0–100 while an OTA is in flight, -1 otherwise. */
  int ota_progress_ = -1;
  std::string pending_build_id_;
  bool welcome_sound_played_ = false;
  uint32_t last_attest_ms_ = 0;
  /** After unlock PIN for a parental-gated app, allow launches until lock. */
  bool parental_session_unlocked_ = false;
  bool parental_pin_for_app_ = false;
  /** Settings → Change PIN: verify current PIN, then OnboardingPinSet/Confirm, then back. */
  bool pin_change_ = false;
  /** Settings → About → Reset: second press confirms. */
  bool reset_armed_ = false;
  HomeApp parental_pending_app_ = HomeApp::Notes;
  /** OnboardingDone / Tips pages (0=ready, 1=rotary, 2=side, 3=power). */
  int tips_page_ = 0;
  /** Settings → Wi‑Fi: 0 = known list, 1 = Add (hotspot / another), 2 = Remove picker. */
  uint8_t wifi_ui_page_ = 0;
  /**
   * Sticky SoftAP / Add-network session. Survives brief screen transitions and blocks
   * Lock (idle, Power glitch, force_lock) until Cancel / success clears it.
   */
  bool wifi_add_session_ = false;
  /**
   * SoftAP RF settle window (ms deadline). Power/Home/Back ignored; wifi_add_flow_active.
   * Covers mode-switch glitches even if start_provision fails and clears the portal.
   */
  uint32_t wifi_rf_guard_until_ms_ = 0;
  /** Paint SoftAP first; start APSTA once display_.busy() clears (or immediately on host). */
  bool wifi_softap_start_pending_ = false;
  /** Preferred SSID held while SoftAP radio start is deferred after paint. */
  std::string wifi_softap_pending_preferred_;
  /** Request ButtonPoller resync + InputMapper nav-gesture discard (main loop). */
  bool input_resync_pending_ = false;
  /** Last attempt to flush offline voice clips via cloud STT. */
  uint32_t last_pending_stt_ms_ = 0;
  /** Empty-transcript count per pending clip; 3 strikes → give up (no endless retries). */
  std::map<std::string, int> stt_failures_;
};

}  // namespace pocket