#pragma once
#include "pocket/app.hpp"

/** Device → Pocket Cloud HTTP (pair sessions + status poll + music sync). */
struct EspCloud : pocket::PlatformCloud {
  std::string create_pair_session(const std::string& device_id) override;
  std::string pair_status(const std::string& code) override;
  void set_device_token(const std::string& token) override;
  std::string take_pair_device_token() override;
  std::string claim_device_token(const std::string& device_id, bool* already_issued) override;
  void refresh_entitlement(pocket::DeviceConfig&) override {}
  std::string stt_transcribe(const std::vector<uint8_t>& pcm) override;
  std::string music_list_json(const std::string& device_id, bool sd_present = false) override;
  std::vector<uint8_t> music_download(const std::string& device_id, const std::string& track_id) override;
  std::string books_list_json(const std::string& device_id) override;
  std::vector<uint8_t> book_download(const std::string& device_id, const std::string& book_id) override;
  bool download_to_file(const char* kind, const std::string& device_id, const std::string& id, const std::string& path,
                        int64_t max_bytes, int64_t* bytes_out) override;
  std::string firmware_latest_json() override;
  std::string device_attest_json(const std::string& device_id) override;
  bool push_device_settings(const std::string& device_id, int volume_percent, int brightness_percent) override;
  std::string http_get_text(const std::string& url) override;
  void set_sd_present(int sd) override { sd_present_ = sd; }
  std::string last_error() const override { return last_error_; }
  std::string device_sync_json(const std::string& device_id, const std::string& body, int* status) override;
  std::string calendar_json(const std::string& device_id) override;
  bool post_backup(const std::string& device_id, const std::string& body) override;
  std::string backup_latest_json(const std::string& device_id) override;
  bool post_reading_progress(const std::string& device_id, const std::string& book_id, int page, int pages,
                             int percent) override;

 private:
  int sd_present_ = -1;
  std::string pair_token_;
  std::string last_error_;
};
