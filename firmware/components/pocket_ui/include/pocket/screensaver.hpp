#pragma once
#include "pocket/canvas.hpp"
#include <cstdint>

namespace pocket {

/** Generative lock/sleep screensavers, designed for the 1-bit e-ink panel
 * (tones are error-diffused or half-toned, line art is pure ink). */
inline constexpr int kScreensaverCount = 12;
/** DeviceConfig::screensaver value for the quiet wordmark-only face. */
inline constexpr uint8_t kScreensaverPlain = 255;

/** Display name for piece `index` (0-based). */
const char* screensaver_name(int index);

/**
 * Paint piece `index` into the rect. `seed` varies composition details so each
 * lock is a fresh print of the same piece; the same seed always draws the same art.
 */
void draw_screensaver(Canvas& c, int index, int x, int y, int w, int h, uint32_t seed);

}  // namespace pocket
