#include "pocket_board/audio.hpp"
#include "pocket_board/i2c_bus.hpp"
#include "pocket_board/pins.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace pocket::board {
namespace {

constexpr const char* TAG = "pocket_audio";
constexpr uint8_t kEs8311Addr = 0x18;
/** 16 kHz × MCLK_MULTIPLE_256 = 4.096 MHz — matches ES8311 coeff table. */
constexpr int kSampleRate = 16000;
constexpr int kMclkHz = kSampleRate * 256;

i2c_master_dev_handle_t es_dev_ = nullptr;
i2s_chan_handle_t tx_ = nullptr;
i2s_chan_handle_t rx_ = nullptr;
bool ready_ = false;
/** Background playback state — the UI loop never blocks on a track. */
volatile bool play_stop_ = false;
volatile bool playing_ = false;
volatile bool capturing_ = false;
/** Raw stereo capture, filled by a dedicated task so e-ink refreshes / network on the UI
 * loop can never starve the I2S DMA ring (which used to drop chunks of speech). */
uint8_t* cap_buf_ = nullptr;
size_t cap_cap_ = 0;
volatile size_t cap_len_ = 0;
volatile bool cap_task_running_ = false;
constexpr size_t kMaxCaptureBytes = 16000 * 2 * 2 * 45;  // 45 s stereo @16 kHz (~2.9 MB, PSRAM)

bool es_wr(uint8_t reg, uint8_t val) {
  if (!es_dev_) return false;
  uint8_t buf[2] = {reg, val};
  const esp_err_t err = i2c_master_transmit(es_dev_, buf, 2, 100);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "ES8311 wr 0x%02x=0x%02x failed: %s", reg, val, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool es_rd(uint8_t reg, uint8_t* val) {
  if (!es_dev_ || !val) return false;
  return i2c_master_transmit_receive(es_dev_, &reg, 1, val, 1, 100) == ESP_OK;
}

bool es_rmw(uint8_t reg, uint8_t clear_mask, uint8_t set_bits) {
  uint8_t v = 0;
  if (!es_rd(reg, &v)) return false;
  v = static_cast<uint8_t>((v & ~clear_mask) | set_bits);
  return es_wr(reg, v);
}

/**
 * Full ES8311 DAC bring-up (Waveshare / Espressif es8311_init sequence).
 * Coeff for mclk=4096000, rate=16000: pre_div=1, pre_multi=0, adc/dac_div=1,
 * fs_mode=0, lrck_h=0, lrck_l=0xff, bclk_div=4, osr=0x10.
 */
bool es_init_regs() {
  // Reset → idle → power-on CSM
  if (!es_wr(0x00, 0x1F)) return false;
  vTaskDelay(pdMS_TO_TICKS(20));
  if (!es_wr(0x00, 0x00)) return false;
  if (!es_wr(0x00, 0x80)) return false;  // Power-on command

  // Clocks: enable all; MCLK from pin (not BCLK)
  if (!es_wr(0x01, 0x3F)) return false;

  // REG02: pre_div-1 in [7:5], pre_multi in [4:3]; keep low 3 bits
  uint8_t reg02 = 0;
  if (!es_rd(0x02, &reg02)) reg02 = 0;
  reg02 = static_cast<uint8_t>((reg02 & 0x07) | ((1 - 1) << 5) | (0 << 3));
  if (!es_wr(0x02, reg02)) return false;

  if (!es_wr(0x03, 0x10)) return false;  // fs_mode | adc_osr
  if (!es_wr(0x04, 0x10)) return false;  // dac_osr
  if (!es_wr(0x05, 0x00)) return false;  // adc_div/dac_div = 1

  uint8_t reg06 = 0;
  if (!es_rd(0x06, &reg06)) reg06 = 0;
  reg06 = static_cast<uint8_t>((reg06 & 0xE0) | (4 - 1));  // bclk_div=4, clear invert
  if (!es_wr(0x06, reg06)) return false;

  uint8_t reg07 = 0;
  if (!es_rd(0x07, &reg07)) reg07 = 0;
  reg07 = static_cast<uint8_t>((reg07 & 0xC0) | 0x00);  // lrck_h
  if (!es_wr(0x07, reg07)) return false;
  if (!es_wr(0x08, 0xFF)) return false;  // lrck_l

  // Slave I2S, 16-bit SDP (resolution code 3 << 2)
  uint8_t reg00 = 0;
  if (!es_rd(0x00, &reg00)) reg00 = 0x80;
  if (!es_wr(0x00, static_cast<uint8_t>(reg00 & ~0x40))) return false;
  if (!es_wr(0x09, 0x0C)) return false;  // SDP in 16-bit
  if (!es_wr(0x0A, 0x0C)) return false;  // SDP out 16-bit

  // Analog / DAC / headphone drive — the audible path
  if (!es_wr(0x0D, 0x01)) return false;  // power up analog
  if (!es_wr(0x0E, 0x02)) return false;  // PGA / ADC modulator
  if (!es_wr(0x12, 0x00)) return false;  // power-up DAC
  if (!es_wr(0x13, 0x10)) return false;  // enable HP drive
  if (!es_wr(0x1C, 0x6A)) return false;  // ADC EQ bypass
  if (!es_wr(0x37, 0x08)) return false;  // DAC EQ bypass

  // Volume ~75% on DAC_REG32; unmute DAC_REG31
  if (!es_wr(0x32, 0xBF)) return false;
  if (!es_rmw(0x31, 0x60, 0x00)) return false;  // clear mute bits 6|5

  // Mic (ADC) path — full Espressif-style ADC bring-up (not just DAC regs).
  // REG14: analog mic1 + PGA (~+18 dB). REG17: ADC digital volume unmuted.
  // REG15/1B: ADC scale / DMIC / HPF defaults from es8311_codec driver.
  if (!es_wr(0x14, 0x1A)) return false;
  if (!es_wr(0x15, 0x40)) return false;
  if (!es_wr(0x1B, 0x0A)) return false;
  if (!es_wr(0x17, 0xBF)) return false;
  // Re-assert ADC modulator power (REG0E) after mic select — needed on some boards
  // where the first 0x0E write races ahead of analog settle.
  if (!es_wr(0x0E, 0x02)) return false;

  return true;
}

bool i2s_init() {
  if (tx_) return true;
  // Duplex channel pair on I2S_NUM_0: tx_ drives the speaker continuously, rx_ (mic ADC data
  // from the same ES8311) is enabled only during a capture window to save power.
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.auto_clear = true;
  // Deeper DMA ring (8 × 480 frames ≈ 240 ms) so a busy moment never drops mic audio.
  chan.dma_desc_num = 8;
  chan.dma_frame_num = 480;
  if (i2s_new_channel(&chan, &tx_, &rx_) != ESP_OK) return false;

  i2s_std_config_t std = {};
  std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  // Stereo L=R avoids mono left/right slot mismatch on this codec path.
  std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  std.gpio_cfg.mclk = static_cast<gpio_num_t>(kPinI2sMclk);
  std.gpio_cfg.bclk = static_cast<gpio_num_t>(kPinI2sBclk);
  std.gpio_cfg.ws = static_cast<gpio_num_t>(kPinI2sWs);
  std.gpio_cfg.dout = static_cast<gpio_num_t>(kPinI2sDout);
  std.gpio_cfg.din = static_cast<gpio_num_t>(kPinI2sDin);
  std.gpio_cfg.invert_flags.mclk_inv = false;
  std.gpio_cfg.invert_flags.bclk_inv = false;
  std.gpio_cfg.invert_flags.ws_inv = false;

  if (i2s_channel_init_std_mode(tx_, &std) != ESP_OK) return false;
  if (rx_ && i2s_channel_init_std_mode(rx_, &std) != ESP_OK) {
    ESP_LOGW(TAG, "mic RX channel init failed — playback still works");
    rx_ = nullptr;
  }
  if (i2s_channel_enable(tx_) != ESP_OK) return false;
  return true;
}

void write_tone(float freq_hz, int ms, float amplitude = 0.28f) {
  if (!tx_) return;
  const int n = (kSampleRate * ms) / 1000;
  // Interleaved stereo [L, R]
  std::vector<int16_t> buf(static_cast<size_t>(n) * 2);
  const int fade = std::min(120, n / 4);
  for (int i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    float env = 1.f;
    if (i < fade) env = static_cast<float>(i) / static_cast<float>(fade);
    if (i > n - fade) env = static_cast<float>(n - i) / static_cast<float>(fade);
    const int16_t s =
        static_cast<int16_t>(amplitude * env * 32767.f * std::sin(2.f * static_cast<float>(M_PI) * freq_hz * t));
    buf[static_cast<size_t>(i) * 2] = s;
    buf[static_cast<size_t>(i) * 2 + 1] = s;
  }
  size_t written = 0;
  i2s_channel_write(tx_, buf.data(), buf.size() * sizeof(int16_t), &written, pdMS_TO_TICKS(ms + 80));
  // Let DMA finish so PA doesn't cut the tail.
  vTaskDelay(pdMS_TO_TICKS(20));
}

}  // namespace

bool audio_init() {
  if (ready_) return true;

  gpio_config_t io = {};
  io.mode = GPIO_MODE_OUTPUT;
  io.pin_bit_mask = 1ULL << kPinPaCtrl;
  gpio_config(&io);
  gpio_set_level(static_cast<gpio_num_t>(kPinPaCtrl), 1);
  vTaskDelay(pdMS_TO_TICKS(100));  // PA settle before codec open

  auto bus = i2c_bus();
  if (!bus) {
    ESP_LOGW(TAG, "no I2C bus");
    return false;
  }
  if (!es_dev_) {
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = kEs8311Addr;
    dev.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(bus, &dev, &es_dev_) != ESP_OK) {
      ESP_LOGW(TAG, "ES8311 device add failed");
      es_dev_ = nullptr;
      return false;
    }
  }

  uint8_t reg = 0x00, val = 0;
  if (i2c_master_transmit_receive(es_dev_, &reg, 1, &val, 1, 100) != ESP_OK) {
    ESP_LOGW(TAG, "ES8311 not responding");
    return false;
  }

  if (!es_init_regs()) {
    ESP_LOGW(TAG, "ES8311 register init failed");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(50));

  if (!i2s_init()) {
    ESP_LOGW(TAG, "I2S init failed");
    return false;
  }
  ready_ = true;
  ESP_LOGI(TAG, "audio ready (mclk=%d rate=%d)", kMclkHz, kSampleRate);
  return true;
}

void audio_play(SoundId id) {
  if (!ready_ && !audio_init()) return;
  if (playing_) return;  // a track owns the I2S channel
  switch (id) {
    case SoundId::Click:
      write_tone(880.f, 45, 0.22f);
      break;
    case SoundId::Welcome:
      write_tone(523.25f, 140, 0.28f);
      write_tone(659.25f, 140, 0.28f);
      write_tone(783.99f, 220, 0.30f);
      break;
    case SoundId::Success:
      write_tone(660.f, 100, 0.26f);
      write_tone(880.f, 160, 0.28f);
      break;
    case SoundId::Attention:
      write_tone(440.f, 90, 0.24f);
      vTaskDelay(pdMS_TO_TICKS(40));
      write_tone(440.f, 90, 0.24f);
      break;
  }
}

namespace {


struct PlayJob {
  FILE* f = nullptr;
  uint16_t channels = 1;
  uint32_t rate = kSampleRate;
  uint32_t data_left = 0;
};

/** Read up to `frames` frames as interleaved stereo int16 (mono duplicated). */
size_t read_frames(PlayJob& job, std::vector<int16_t>& dst, size_t frames) {
  const size_t frame_bytes = sizeof(int16_t) * job.channels;
  size_t want = std::min(frames, static_cast<size_t>(job.data_left / frame_bytes));
  if (want == 0) return 0;
  std::vector<int16_t> raw(want * job.channels);
  const size_t got = std::fread(raw.data(), frame_bytes, want, job.f);
  job.data_left -= static_cast<uint32_t>(got * frame_bytes);
  for (size_t i = 0; i < got; ++i) {
    const int16_t l = raw[i * job.channels];
    const int16_t r = job.channels == 2 ? raw[i * 2 + 1] : l;
    dst.push_back(l);
    dst.push_back(r);
  }
  return got;
}

void play_task(void* arg) {
  PlayJob* job = static_cast<PlayJob*>(arg);
  // Linear-interpolating resampler to the codec's fixed 16 kHz clock, so 44.1/48 kHz
  // files play at the right speed and pitch (they used to play ~3x slow).
  const double step = static_cast<double>(job->rate) / kSampleRate;
  std::vector<int16_t> in;  // interleaved stereo frames
  in.reserve(4096);
  double pos = 0;  // input frame position relative to in[0]
  bool eof = false;
  std::vector<int16_t> out(512 * 2);
  while (!play_stop_) {
    size_t frames_in = in.size() / 2;
    while (!eof && frames_in < static_cast<size_t>(pos) + 2 + 512 * step) {
      if (read_frames(*job, in, 1024) == 0) eof = true;
      frames_in = in.size() / 2;
    }
    size_t n_out = 0;
    while (n_out < 512) {
      const size_t idx = static_cast<size_t>(pos);
      if (idx + 1 >= frames_in) break;
      const double f = pos - static_cast<double>(idx);
      out[n_out * 2] = static_cast<int16_t>(in[idx * 2] * (1.0 - f) + in[(idx + 1) * 2] * f);
      out[n_out * 2 + 1] = static_cast<int16_t>(in[idx * 2 + 1] * (1.0 - f) + in[(idx + 1) * 2 + 1] * f);
      ++n_out;
      pos += step;
    }
    if (n_out == 0) break;  // end of data
    size_t written = 0;
    i2s_channel_write(tx_, out.data(), n_out * 2 * sizeof(int16_t), &written, pdMS_TO_TICKS(500));
    // Drop consumed input so the buffer stays small.
    const size_t consumed = static_cast<size_t>(pos);
    if (consumed > 1024) {
      in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(consumed * 2));
      pos -= static_cast<double>(consumed);
    }
  }
  std::fclose(job->f);
  delete job;
  playing_ = false;
  vTaskDelete(nullptr);
}

}  // namespace

bool audio_play_wav_file(const char* path) {
  if (!path || !*path) return false;
  if (!ready_ && !audio_init()) return false;
  audio_stop();
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    ESP_LOGW(TAG, "open failed: %s", path);
    return false;
  }
  // Walk RIFF chunks instead of assuming a 44-byte header (LIST/INFO chunks used to
  // play as a burst of noise, and a non-16 kHz file played at the wrong speed).
  uint8_t riff[12] = {};
  if (std::fread(riff, 1, 12, f) != 12 || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
    ESP_LOGW(TAG, "not WAV: %s", path);
    std::fclose(f);
    return false;
  }
  uint16_t fmt_tag = 0, channels = 0, bits = 0;
  uint32_t rate = 0, data_size = 0;
  bool have_fmt = false, have_data = false;
  for (int guard = 0; guard < 32 && !have_data; ++guard) {
    uint8_t ch[8];
    if (std::fread(ch, 1, 8, f) != 8) break;
    const uint32_t size = static_cast<uint32_t>(ch[4] | (ch[5] << 8) | (ch[6] << 16) | (ch[7] << 24));
    if (std::memcmp(ch, "fmt ", 4) == 0) {
      uint8_t fmt[16] = {};
      if (size < 16 || std::fread(fmt, 1, 16, f) != 16) break;
      fmt_tag = static_cast<uint16_t>(fmt[0] | (fmt[1] << 8));
      channels = static_cast<uint16_t>(fmt[2] | (fmt[3] << 8));
      rate = static_cast<uint32_t>(fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24));
      bits = static_cast<uint16_t>(fmt[14] | (fmt[15] << 8));
      have_fmt = true;
      if (size > 16) std::fseek(f, static_cast<long>(size - 16 + (size & 1)), SEEK_CUR);
    } else if (std::memcmp(ch, "data", 4) == 0) {
      data_size = size;
      have_data = true;
    } else {
      std::fseek(f, static_cast<long>(size + (size & 1)), SEEK_CUR);
    }
  }
  if (!have_fmt || !have_data || fmt_tag != 1 || bits != 16 || (channels != 1 && channels != 2) || rate < 4000 ||
      rate > 96000) {
    ESP_LOGW(TAG, "unsupported WAV (fmt=%u ch=%u bits=%u rate=%u)", fmt_tag, channels, bits,
             static_cast<unsigned>(rate));
    std::fclose(f);
    return false;
  }
  auto* job = new PlayJob{f, channels, rate, data_size};
  play_stop_ = false;
  playing_ = true;
  if (xTaskCreate(play_task, "pocket_play", 6144, job, 5, nullptr) != pdPASS) {
    playing_ = false;
    std::fclose(f);
    delete job;
    return false;
  }
  return true;
}

bool audio_is_playing() { return playing_; }

void audio_stop() {
  if (!playing_) return;
  play_stop_ = true;
  for (int i = 0; i < 60 && playing_; ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

void audio_set_volume(uint8_t percent) {
  if (percent > 100) percent = 100;
  if (!ready_ && !audio_init()) return;
  // DAC_REG32 is linear 0..255; keep the same ~0.75 ceiling used at bring-up as "100%" so the
  // default doesn't clip on the small onboard speaker.
  const uint8_t reg = static_cast<uint8_t>((static_cast<int>(percent) * 0xBF) / 100);
  es_wr(0x32, reg);
}

bool audio_mic_supported() {
  if (!ready_ && !audio_init()) return false;
  return rx_ != nullptr;
}

void capture_task(void*) {
  uint8_t chunk[4096];
  while (capturing_) {
    size_t nread = 0;
    if (i2s_channel_read(rx_, chunk, sizeof(chunk), &nread, pdMS_TO_TICKS(100)) == ESP_OK && nread > 0) {
      const size_t len = cap_len_;
      const size_t take = std::min(nread, cap_cap_ - len);
      if (take > 0) {
        std::memcpy(cap_buf_ + len, chunk, take);
        cap_len_ = len + take;
      }
    }
  }
  cap_task_running_ = false;
  vTaskDelete(nullptr);
}

void audio_capture_start() {
  if (!audio_mic_supported()) return;
  if (capturing_ || cap_task_running_) return;
  audio_stop();  // recording mutes the speaker — end any track cleanly first
  if (!cap_buf_) {
    // Prefer PSRAM for the long buffer; fall back to a short internal one.
    cap_buf_ = static_cast<uint8_t*>(heap_caps_malloc(kMaxCaptureBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    cap_cap_ = kMaxCaptureBytes;
    if (!cap_buf_) {
      cap_cap_ = 16000 * 2 * 2 * 4;
      cap_buf_ = static_cast<uint8_t*>(malloc(cap_cap_));
      if (!cap_buf_) {
        cap_cap_ = 0;
        ESP_LOGE(TAG, "no memory for mic capture");
        return;
      }
    }
  }
  cap_len_ = 0;
  // Mute the speaker amp while recording so PA feedback doesn't swamp the mic.
  gpio_set_level(static_cast<gpio_num_t>(kPinPaCtrl), 0);
  // Re-assert mic PGA/volume each capture (safe if already set).
  es_wr(0x14, 0x1A);
  es_wr(0x17, 0xBF);
  es_wr(0x0E, 0x02);
  if (i2s_channel_enable(rx_) != ESP_OK) {
    gpio_set_level(static_cast<gpio_num_t>(kPinPaCtrl), 1);
    return;
  }
  // Let the ADC settle; discard the first noisy chunk.
  vTaskDelay(pdMS_TO_TICKS(40));
  uint8_t discard[512];
  size_t nread = 0;
  (void)i2s_channel_read(rx_, discard, sizeof(discard), &nread, pdMS_TO_TICKS(30));
  capturing_ = true;
  cap_task_running_ = true;
  if (xTaskCreatePinnedToCore(capture_task, "mic_cap", 6144, nullptr, 6, nullptr, 0) != pdPASS) {
    capturing_ = false;
    cap_task_running_ = false;
    i2s_channel_disable(rx_);
    gpio_set_level(static_cast<gpio_num_t>(kPinPaCtrl), 1);
    ESP_LOGE(TAG, "mic task start failed");
  }
}

void audio_capture_poll() {
  // Capture drains on its own task; nothing to do on the UI loop.
}

MicCaptureResult audio_capture_stop() {
  MicCaptureResult result;
  if (!capturing_) return result;
  // Keep the last ~120 ms in flight (end of the final word), then stop the task.
  vTaskDelay(pdMS_TO_TICKS(120));
  capturing_ = false;
  for (int i = 0; i < 30 && cap_task_running_; ++i) vTaskDelay(pdMS_TO_TICKS(10));
  if (rx_) i2s_channel_disable(rx_);
  gpio_set_level(static_cast<gpio_num_t>(kPinPaCtrl), 1);

  const size_t cap_len = cap_len_;
  result.ok = cap_len > 0;
  if (result.ok) {
    // Stereo interleaved 16-bit. Pick the louder channel (or average when similar) —
    // some board revisions wire the analog mic into R instead of L.
    const auto* samples = reinterpret_cast<const int16_t*>(cap_buf_);
    const size_t n_frames = cap_len / (2 * sizeof(int16_t));
    double sum_l = 0.0, sum_r = 0.0;
    for (size_t i = 0; i < n_frames; ++i) {
      const double l = samples[i * 2];
      const double r = samples[i * 2 + 1];
      sum_l += l * l;
      sum_r += r * r;
    }
    const bool use_right = sum_r > sum_l * 1.25;
    const bool blend = !use_right && sum_l <= sum_r * 1.25 && sum_r > 0;
    // Downmix in place (mono[i] only overwrites frames already read).
    auto* mono = reinterpret_cast<int16_t*>(cap_buf_);
    double sum_sq = 0.0;
    for (size_t i = 0; i < n_frames; ++i) {
      int16_t s;
      if (use_right) {
        s = samples[i * 2 + 1];
      } else if (blend) {
        const int32_t avg = (static_cast<int32_t>(samples[i * 2]) + samples[i * 2 + 1]) / 2;
        s = static_cast<int16_t>(avg);
      } else {
        s = samples[i * 2];
      }
      mono[i] = s;
      sum_sq += static_cast<double>(s) * static_cast<double>(s);
    }
    // Trim leading/trailing silence (20 ms windows) — smaller upload, faster transcript.
    constexpr size_t kWin = 320;
    const size_t n_win = n_frames / kWin;
    size_t first = 0, last = n_frames;
    if (n_win > 4) {
      std::vector<float> e(n_win);
      float peak = 0.f;
      for (size_t w = 0; w < n_win; ++w) {
        double acc = 0.0;
        for (size_t k = 0; k < kWin; ++k) {
          const double v = mono[w * kWin + k];
          acc += v * v;
        }
        e[w] = static_cast<float>(std::sqrt(acc / kWin));
        peak = std::max(peak, e[w]);
      }
      const float thr = std::max(120.f, peak * 0.08f);
      size_t w0 = 0, w1 = n_win;
      while (w0 < n_win && e[w0] < thr) ++w0;
      while (w1 > w0 && e[w1 - 1] < thr) --w1;
      if (w0 < w1) {
        constexpr size_t kPadWin = 12;  // keep 240 ms either side so soft consonants survive
        first = (w0 > kPadWin ? w0 - kPadWin : 0) * kWin;
        last = std::min(n_frames, (w1 + kPadWin) * kWin);
      }
    }
    result.pcm.resize((last - first) * sizeof(int16_t));
    std::memcpy(result.pcm.data(), mono + first, result.pcm.size());
    const double rms = n_frames ? std::sqrt(sum_sq / static_cast<double>(n_frames)) : 0.0;
    // 32767 full-scale → percent scale tuned so normal speech reads ~15-40%.
    result.level_percent = static_cast<int>(std::min(100.0, (rms / 32767.0) * 400.0));
    // Treat near-silence as a failed capture so Notes can prompt the user to retry.
    if (result.level_percent < 2) {
      result.ok = false;
      result.pcm.clear();
    }
  }
  cap_len_ = 0;
  return result;
}

}  // namespace pocket::board
