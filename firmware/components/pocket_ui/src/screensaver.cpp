/**
 * Pocket screensavers — generative prints for a 1-bit e-ink panel.
 *
 * The EPD driver thresholds every frame to black/white, so nothing here relies
 * on the canvas's mid grays. Tonal pieces compute a continuous ink value per
 * pixel (0 = paper, 1 = ink) and error-diffuse it (serpentine Floyd–Steinberg,
 * two row buffers — no full-frame float buffer). Line pieces draw pure ink.
 *
 * Everything is single-precision and allocation-light so a lock paints in well
 * under a second on the ESP32-S3.
 */
#include "pocket/screensaver.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace pocket {
namespace {

constexpr float kPi = 3.14159265f;

// --- Deterministic noise -------------------------------------------------

inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

inline uint32_t hash2(int x, int y, uint32_t seed) {
  return hash32(static_cast<uint32_t>(x) * 0x9E3779B1u ^ hash32(static_cast<uint32_t>(y) + seed * 0x85EBCA77u));
}

/** Uniform 0..1 from a hash. */
inline float h01(uint32_t h) { return static_cast<float>(h & 0xFFFFFF) / 16777215.0f; }

struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(hash32(seed + 0x51ED270Bu)) {}
  float next() {
    s = hash32(s + 0x6D2B79F5u);
    return h01(s);
  }
  float range(float a, float b) { return a + (b - a) * next(); }
};

inline float smooth(float t) { return t * t * (3.0f - 2.0f * t); }
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float smoothstep(float a, float b, float x) { return smooth(clamp01((x - a) / (b - a))); }

float value_noise(float x, float y, uint32_t seed) {
  const int xi = static_cast<int>(std::floor(x));
  const int yi = static_cast<int>(std::floor(y));
  const float fx = smooth(x - static_cast<float>(xi));
  const float fy = smooth(y - static_cast<float>(yi));
  const float a = h01(hash2(xi, yi, seed));
  const float b = h01(hash2(xi + 1, yi, seed));
  const float c = h01(hash2(xi, yi + 1, seed));
  const float d = h01(hash2(xi + 1, yi + 1, seed));
  return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

float fbm(float x, float y, uint32_t seed, int octaves = 4) {
  float sum = 0, amp = 0.5f, norm = 0;
  for (int o = 0; o < octaves; ++o) {
    sum += amp * value_noise(x, y, seed + static_cast<uint32_t>(o) * 131u);
    norm += amp;
    x = x * 2.03f + 17.1f;
    y = y * 2.03f - 9.7f;
    amp *= 0.5f;
  }
  return sum / norm;
}

// --- Dithering -----------------------------------------------------------

/** Serpentine Floyd–Steinberg over a tone function ink(x, y) ∈ [0,1]. */
template <typename F>
void diffuse(Canvas& c, int x0, int y0, int w, int h, F ink) {
  std::vector<float> cur(static_cast<size_t>(w) + 2, 0.0f), nxt(static_cast<size_t>(w) + 2, 0.0f);
  for (int y = 0; y < h; ++y) {
    std::fill(nxt.begin(), nxt.end(), 0.0f);
    const bool ltr = (y & 1) == 0;
    for (int i = 0; i < w; ++i) {
      const int x = ltr ? i : (w - 1 - i);
      const int k = x + 1;
      const float v = clamp01(ink(x, y)) + cur[k];
      const float q = v >= 0.5f ? 1.0f : 0.0f;
      const float e = v - q;
      if (q > 0) c.set_pixel(x0 + x, y0 + y, Gray::G0);
      const int dir = ltr ? 1 : -1;
      cur[k + dir] += e * (7.0f / 16.0f);
      nxt[k - dir] += e * (3.0f / 16.0f);
      nxt[k] += e * (5.0f / 16.0f);
      nxt[k + dir] += e * (1.0f / 16.0f);
    }
    std::swap(cur, nxt);
  }
}

/** Classic 45° halftone screen: dot area tracks ink(x, y) sampled at each cell. */
template <typename F>
void halftone(Canvas& c, int x0, int y0, int w, int h, float cell, F ink) {
  const float ca = 0.70710678f, sa = 0.70710678f;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      // Rotate into screen space, find cell center, rotate back to sample tone there.
      const float u = (x * ca + y * sa) / cell;
      const float v = (-x * sa + y * ca) / cell;
      const float cu = std::floor(u) + 0.5f;
      const float cv = std::floor(v) + 0.5f;
      const float sx = (cu * ca - cv * sa) * cell;
      const float sy = (cu * sa + cv * ca) * cell;
      const float t = clamp01(ink(static_cast<int>(sx), static_cast<int>(sy)));
      // Dot radius (in cell units) for area coverage t; past ~0.78 dots merge.
      const float r = std::sqrt(t / kPi) * 1.02f;
      const float du = u - cu, dv = v - cv;
      if (du * du + dv * dv <= r * r) c.set_pixel(x0 + x, y0 + y, Gray::G0);
    }
  }
}

// --- Pieces --------------------------------------------------------------

/** Ranges — mountain ridges receding in mist, darker as they come forward. */
void piece_ranges(Canvas& c, int X, int Y, int W, int H, uint32_t seed) {
  constexpr int kLayers = 6;
  Rng r(seed);
  std::vector<float> ridge(static_cast<size_t>(kLayers * W));
  float base[kLayers];
  for (int k = 0; k < kLayers; ++k) {
    base[k] = H * (0.36f + 0.105f * k);
    const float amp = 60.0f + 18.0f * k;
    const float s = 0.0042f + 0.0012f * k;
    const uint32_t ns = seed + 97u * static_cast<uint32_t>(k + 1);
    for (int x = 0; x < W; ++x) {
      // Ridged fbm: sharp summits, rounded valleys.
      float h = 0, amp_o = 1, norm = 0, fx = x * s;
      for (int o = 0; o < 4; ++o) {
        const float n = 1.0f - std::fabs(2.0f * value_noise(fx, k * 5.3f + o, ns + o) - 1.0f);
        h += amp_o * n * n;
        norm += amp_o;
        amp_o *= 0.5f;
        fx *= 2.05f;
      }
      ridge[static_cast<size_t>(k * W + x)] = base[k] - amp * (h / norm);
    }
  }
  const float sun_x = W * r.range(0.6f, 0.78f), sun_y = H * r.range(0.13f, 0.2f);
  diffuse(c, X, Y, W, H, [&](int x, int y) {
    const float fy = static_cast<float>(y);
    for (int k = kLayers - 1; k >= 0; --k) {
      const float top = ridge[static_cast<size_t>(k * W + x)];
      if (fy >= top) {
        const float ink = 0.14f + 0.15f * k;
        // Mist pools in each valley floor, thinning toward the summits.
        const float mist = smoothstep(top + 10.0f, base[k] + 40.0f, fy);
        return ink * (1.0f - 0.55f * mist) + (k == kLayers - 1 ? 0.25f * mist : 0.0f);
      }
    }
    const float dx = x - sun_x, dy = fy - sun_y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d < 34.0f) return 0.0f;
    if (d < 37.0f) return 0.9f;
    return 0.04f + 0.06f * (fy / (H * 0.4f));
  });
}

/** Contours — a topographic sheet; every fifth line is an index contour. */
void piece_contours(Canvas& c, int X, int Y, int W, int H, uint32_t seed) {
  Rng r(seed);
  const float s = r.range(0.0032f, 0.0042f);
  const float px = W * r.range(0.3f, 0.7f), py = H * r.range(0.3f, 0.6f);
  constexpr float kLevels = 26.0f;
  auto field = [&](int x, int y) {
    const float dx = (x - px) / W, dy = (y - py) / H;
    const float peak = std::exp(-(dx * dx + dy * dy) * 5.0f);
    return 0.65f * fbm(x * s, y * s, seed, 4) + 0.45f * peak;
  };
  // The field is smooth at pixel scale: sample even columns, interpolate odd ones.
  auto fill_row = [&](std::vector<float>& row, int y) {
    for (int x = 0; x < W; x += 2) row[static_cast<size_t>(x)] = field(x, y);
    for (int x = 1; x < W; x += 2)
      row[static_cast<size_t>(x)] = x + 1 < W ? 0.5f * (row[static_cast<size_t>(x - 1)] + row[static_cast<size_t>(x + 1)])
                                              : field(x, y);
  };
  std::vector<float> prev(static_cast<size_t>(W)), cur(static_cast<size_t>(W)), next(static_cast<size_t>(W));
  fill_row(prev, -1);
  fill_row(cur, 0);
  for (int y = 0; y < H; ++y) {
    fill_row(next, y + 1);
    for (int x = 1; x < W - 1; ++x) {
      const float v = cur[static_cast<size_t>(x)] * kLevels;
      const float gx = (cur[static_cast<size_t>(x + 1)] - cur[static_cast<size_t>(x - 1)]) * 0.5f * kLevels;
      const float gy = (next[static_cast<size_t>(x)] - prev[static_cast<size_t>(x)]) * 0.5f * kLevels;
      const float g = std::sqrt(gx * gx + gy * gy) + 1e-4f;
      const float lvl = std::floor(v + 0.5f);
      const float dist = std::fabs(v - lvl) / g;  // pixels to nearest isoline
      const bool index = (static_cast<int>(lvl) % 5) == 0;
      if (dist < (index ? 1.25f : 0.62f)) c.set_pixel(X + x, Y + y, Gray::G0);
    }
    std::swap(prev, cur);
    std::swap(cur, next);
  }
  // Survey marks — a few quiet crosses on the sheet.
  for (int i = 0; i < 5; ++i) {
    const int cx = X + static_cast<int>(r.range(40, W - 40));
    const int cy = Y + static_cast<int>(r.range(40, H - 40));
    c.fill_rect(cx - 7, cy - 7, 15, 15, Gray::G3);
    c.hline(cx - 6, cy, 13, Gray::G0);
    c.vline(cx, cy - 6, 13, Gray::G0);
  }
}

/** Moonrise — a lit moon over still water, stars, and a broken reflection. */
void piece_moon(Canvas& c, int X, int Y, int W, int H, uint32_t seed) {
  Rng r(seed);
  const float mx = W * r.range(0.42f, 0.58f);
  const float my = H * 0.38f;
  const float R = 138.0f;
  const float horizon = H * 0.80f;
  // Light from upper right, slightly in front: a waxing gibbous.
  float lx = r.range(0.55f, 0.85f), ly = -0.25f, lz = 0.55f;
  const float ln = std::sqrt(lx * lx + ly * ly + lz * lz);
  lx /= ln;
  ly /= ln;
  lz /= ln;
  struct Crater { float x, y, r; };
  Crater craters[22];
  for (auto& cr : craters) {
    const float a = r.range(0, 2 * kPi);
    const float d = std::sqrt(r.next()) * 0.92f;
    cr = {std::cos(a) * d, std::sin(a) * d, r.range(0.035f, 0.11f)};
  }
  diffuse(c, X, Y, W, H, [&](int x, int y) {
    const float fy = static_cast<float>(y);
    if (fy >= horizon) {
      // Water: dark, with a shimmering column under the moon.
      const float d = std::fabs(x - mx);
      const float row = std::sin(fy * 0.9f + value_noise(x * 0.05f, fy * 0.3f, seed) * 6.0f);
      const float width = 26.0f + (fy - horizon) * 0.9f;
      if (d < width && row > 0.55f - 0.4f * (1.0f - d / width)) return 0.1f;
      return 0.93f;
    }
    const float nx = (x - mx) / R, ny = (fy - my) / R;
    const float rr = nx * nx + ny * ny;
    if (rr <= 1.0f) {
      const float nz = std::sqrt(1.0f - rr);
      float lam = std::max(0.0f, nx * lx + ny * ly + nz * lz);
      float albedo = 0.92f - 0.38f * smoothstep(0.48f, 0.68f, fbm(nx * 2.2f + 3, ny * 2.2f, seed + 7, 4));
      for (const auto& cr : craters) {
        const float dx = nx - cr.x, dy = ny - cr.y;
        const float d = std::sqrt(dx * dx + dy * dy) / cr.r;
        if (d < 1.0f) albedo -= 0.22f * (1.0f - d * d);
        else if (d < 1.25f) albedo += 0.12f * (1.25f - d) * 4.0f * (dx * lx + dy * ly > 0 ? 1.0f : -0.5f);
      }
      const float lit = clamp01(lam * albedo * 1.15f);
      return 0.97f - 0.92f * lit;  // dark side keeps a faint earthshine
    }
    // Sky: hash-placed stars on near-black.
    const uint32_t hsh = hash2(x, y, seed + 99);
    if ((hsh & 0x3FF) == 0) return 0.0f;
    return 0.96f;
  });
  // A few brighter four-point stars, kept clear of the moon.
  for (int i = 0; i < 9; ++i) {
    const int sx = static_cast<int>(r.range(16, W - 16));
    const int sy = static_cast<int>(r.range(14, horizon - 20));
    const float dx = sx - mx, dy = sy - my;
    if (dx * dx + dy * dy < (R + 30) * (R + 30)) continue;
    const int a = (i % 3 == 0) ? 4 : 2;
    c.hline(X + sx - a, Y + sy, 2 * a + 1, Gray::G3);
    c.vline(X + sx, Y + sy - a, 2 * a + 1, Gray::G3);
  }
}

/** Fir & fog — ranks of conifers stepping out of valley mist. */
void piece_pines(Canvas& c, int X, int Y, int W, int H, uint32_t seed) {
  constexpr int kRows = 5;
  Rng r(seed);
  std::vector<float> sky(static_cast<size_t>(kRows * W), 1e9f);
  float base[kRows];
  for (int k = 0; k < kRows; ++k) {
    base[k] = H * (0.50f + 0.125f * k);
    const float spacing = 10.0f + 7.0f * k;
    const float th = 70.0f + 42.0f * k;
    for (float cx = -spacing; cx < W + spacing; cx += spacing * r.range(0.55f, 1.25f)) {
      const float height = th * r.range(0.65f, 1.25f);
      const float top = base[k] - height;
      const float half = height * r.range(0.20f, 0.27f);
      const float tiers = r.range(5.0f, 8.0f);
      for (int t = 0; t < static_cast<int>(height); ++t) {
        const float u = static_cast<float>(t) / height;
        const float frac = u * tiers - std::floor(u * tiers);
        const float hw = half * u * (0.72f + 0.38f * frac) + 1.0f;
        const int x0 = static_cast<int>(cx - hw), x1 = static_cast<int>(cx + hw);
        for (int x = std::max(0, x0); x <= std::min(W - 1, x1); ++x) {
          float& s = sky[static_cast<size_t>(k * W + x)];
          s = std::min(s, top + t);
        }
      }
    }
    for (int x = 0; x < W; ++x) {
      float& s = sky[static_cast<size_t>(k * W + x)];
      s = std::min(s, base[k] + 4.0f * std::sin(x * 0.02f + k));
    }
  }
  const float ridge_amp = 40.0f;
  const float sun_x = W * r.range(0.25f, 0.4f), sun_y = H * 0.20f;
  diffuse(c, X, Y, W, H, [&](int x, int y) {
    const float fy = static_cast<float>(y);
    for (int k = kRows - 1; k >= 0; --k) {
      if (fy >= sky[static_cast<size_t>(k * W + x)]) {
        const float row_ink = 0.30f + 0.16f * k;
        // Mist pools at each rank's foot and thins upward.
        const float mist = smoothstep(base[k] - 90.0f, base[k] + 8.0f, fy);
        return row_ink * (1.0f - 0.72f * mist) + (k == kRows - 1 ? 0.2f * mist : 0.0f);
      }
    }
    // Far mountain.
    const float m = H * 0.42f - ridge_amp * (0.6f + std::sin(x * 0.011f + 1.3f) * 0.5f) -
                    40.0f * fbm(x * 0.008f, 0.5f, seed, 3);
    if (fy >= m) return 0.16f * (1.0f - 0.6f * smoothstep(m, H * 0.50f, fy));
    const float dx = x - sun_x, dy = fy - sun_y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d < 30.0f) return 0.0f;
    if (d < 33.0f) return 0.55f;
    return 0.03f + 0.05f * (fy / H);
  });
}

/** Currents — evenly spaced streamlines through a flow field; a sun in heavier strokes. */
void piece_flow(Canvas& c, int X, int Y, int W, int H, uint32_t seed) {
  Rng r(seed);
  const float s = r.range(0.0022f, 0.0032f);
  constexpr int kCell = 5;
  const int gw = W / kCell + 1, gh = H / kCell + 1;
  std::vector<uint8_t> used(static_cast<size_t>(gw * gh), 0);
  const float sun_x = W * r.range(0.35f, 0.65f), sun_y = H * r.range(0.32f, 0.45f), sun_r = 120.0f;
  auto angle = [&](float x, float y) { return fbm(x * s, y * s, seed, 3) * 4.0f * kPi; };
  std::vector<std::pair<float, float>> pts;
  for (int gy = 0; gy < H; gy += 9) {
    for (int gx = 0; gx < W; gx += 9) {
      const float sx = gx + r.range(0, 8), sy = gy + r.range(0, 8);
      const int ci = static_cast<int>(sx) / kCell + (static_cast<int>(sy) / kCell) * gw;
      if (ci < 0 || ci >= gw * gh || used[static_cast<size_t>(ci)]) continue;
      pts.clear();
      for (int dir = -1; dir <= 1; dir += 2) {
        float x = sx, y = sy;
        for (int step = 0; step < 90; ++step) {
          const float a = angle(x, y);
          x += std::cos(a) * 1.6f * dir;
          y += std::sin(a) * 1.6f * dir;
          if (x < 0 || y < 0 || x >= W || y >= H) break;
          const int cj = static_cast<int>(x) / kCell + (static_cast<int>(y) / kCell) * gw;
          if (used[static_cast<size_t>(cj)] == 1 && step > 2) break;
          pts.emplace_back(x, y);
        }
      }
      if (pts.size() < 14) continue;
      for (const auto& p : pts) {
        const int cj = static_cast<int>(p.first) / kCell + (static_cast<int>(p.second) / kCell) * gw;
        used[static_cast<size_t>(cj)] = 1;
        const float dx = p.first - sun_x, dy = p.second - sun_y;
        const bool in_sun = dx * dx + dy * dy < sun_r * sun_r;
        const int px = X + static_cast<int>(p.first), py = Y + static_cast<int>(p.second);
        c.set_pixel(px, py, Gray::G0);
        if (in_sun && p.first + 1 < W && p.second + 1 < H) {
          c.set_pixel(px + 1, py, Gray::G0);
          c.set_pixel(px, py + 1, Gray::G0);
          c.set_pixel(px + 1, py + 1, Gray::G0);
        }
      }
    }
  }
}

/** Halftone sun — a printed sunset: banded sun on a deepening sky, dark sea, broken reflection. */
void piece_halftone(Canvas& c, int X, int Y, int W, int H, uint32_t seed) {
  Rng r(seed);
  const float cx = W * r.range(0.44f, 0.56f), R = 150.0f;
  const float horizon = H * 0.64f;
  const float cy = horizon - R * r.range(0.85f, 1.05f);
  halftone(c, X, Y, W, H, 9.0f, [&](int x, int y) {
    const float fy = static_cast<float>(y);
    if (fy >= horizon) {
      const float depth = (fy - horizon) / (H - horizon);
      const float d = std::fabs(x - cx);
      // Reflection: short bright dashes, narrowing and thinning toward the viewer.
      const float half = R * (0.95f - 0.6f * depth) + 20.0f * (value_noise(fy * 0.08f, 1, seed) - 0.5f);
      const float wave = std::fmod(fy - horizon, 12.0f + 10.0f * depth);
      if (d < half && wave < 6.0f - 3.0f * depth) return 0.08f;
      return 0.80f + 0.12f * depth;
    }
    const float dx = x - cx, dy = fy - cy;
    if (dx * dx + dy * dy <= R * R) {
      const float u = (fy - cy) / R;  // -1 top … +1 bottom
      if (u > 0.0f) {
        // Paper bands cut the lower half, widening toward the horizon.
        const float period = 22.0f;
        const float band = std::fmod(fy - cy, period);
        if (band < 3.0f + 9.0f * u) return 0.78f - 0.42f * smoothstep(0, horizon, fy);
      }
      return 0.0f;
    }
    // Sky darkens upward; a paler band hugs the horizon.
    return 0.78f - 0.42f * smoothstep(0, horizon, fy);
  });
}

/** Tide lines — a sea drawn as swells receding toward a bright horizon. */
void piece_tide(Canvas& c, int X, int Y, int W, int H, uint32_t seed) {
  Rng r(seed);
  const float horizon = H * r.range(0.30f, 0.36f);
  // Sun outline sitting on the horizon.
  const int sx = static_cast<int>(W * r.range(0.6f, 0.75f));
  c.stroke_circle(X + sx, Y + static_cast<int>(horizon) - 4, 46, Gray::G0, 3);
  c.fill_rect(X, Y + static_cast<int>(horizon) - 3, W, 400, Gray::G3);
  // A few long cloud strokes.
  for (int i = 0; i < 4; ++i) {
    const int y = static_cast<int>(horizon * r.range(0.25f, 0.8f));
    const int x0 = static_cast<int>(r.range(-40, W * 0.6f));
    const int len = static_cast<int>(r.range(90, 220));
    c.dotted_hline(X + std::max(0, x0), Y + y, std::min(len, W - std::max(0, x0)), Gray::G0, 2);
  }
  c.hline(X, Y + static_cast<int>(horizon), W, Gray::G0);
  constexpr int kLines = 34;
  for (int j = 1; j <= kLines; ++j) {
    const float t = static_cast<float>(j) / kLines;
    const float y0 = horizon + (H - horizon) * std::pow(t, 1.7f);
    const float amp = 1.0f + 12.0f * t * t;
    const float f = 0.05f - 0.035f * t;
    const float ph = r.range(0, 2 * kPi);
    const int thick = 1 + static_cast<int>(3.0f * t);
    int prev_y = 0;
    for (int x = 0; x < W; ++x) {
      const float n = fbm(x * 0.01f, j * 0.7f, seed, 2) - 0.5f;
      // Asymmetric swell: sharp crests, long troughs.
      const float sw = std::sin(x * f + ph);
      const float crest = sw > 0 ? std::pow(sw, 3.0f) : sw * 0.35f;
      const int y = static_cast<int>(y0 - amp * crest + 10.0f * t * n);
      // Glitter path under the sun: break the line.
      if (std::fabs(x - sx) < 26.0f * (1 - t) + 6 && ((x + j * 7) % 9) < 4) {
        prev_y = y;
        continue;
      }
      const int a = x == 0 ? y : std::min(prev_y, y);
      const int b = std::min(H - 1, (x == 0 ? y : std::max(prev_y, y)) + thick - 1);
      if (a <= b) c.fill_rect(X + x, Y + a, 1, b - a + 1, Gray::G0);
      prev_y = y;
    }
  }
}

/** Fern — a fractal fern (iterated function system) leaning into a pale sky. */
void piece_fern(Canvas& c, int X, int Y, int W, int H, uint32_t seed) {
  Rng r(seed);
  // Soft sky: dithered wash, lighter toward the top.
  diffuse(c, X, Y, W, H, [&](int, int y) { return 0.04f + 0.10f * (static_cast<float>(y) / H); });
  auto fern = [&](float cx, float base, float scale, float lean, int points, uint32_t s2) {
    Rng fr(s2);
    float x = 0, y = 0;
    const float cl = std::cos(lean), sl = std::sin(lean);
    for (int i = 0; i < points; ++i) {
      const float p = fr.next();
      float nx, ny;
      if (p < 0.01f) {
        nx = 0;
        ny = 0.16f * y;
      } else if (p < 0.86f) {
        nx = 0.85f * x + 0.04f * y;
        ny = -0.04f * x + 0.85f * y + 1.6f;
      } else if (p < 0.93f) {
        nx = 0.20f * x - 0.26f * y;
        ny = 0.23f * x + 0.22f * y + 1.6f;
      } else {
        nx = -0.15f * x + 0.28f * y;
        ny = 0.26f * x + 0.24f * y + 0.44f;
      }
      x = nx;
      y = ny;
      if (i < 20) continue;
      const float rx = x * cl - y * sl, ry = x * sl + y * cl;
      const int px = static_cast<int>(cx + rx * scale);
      const int py = static_cast<int>(base - ry * scale);
      if (px >= 0 && px < W && py >= 0 && py < H) c.set_pixel(X + px, Y + py, Gray::G0);
    }
  };
  fern(W * r.range(0.45f, 0.58f), H * 0.98f, H / 10.8f, r.range(-0.22f, 0.05f), 220000, seed + 1);
  fern(W * r.range(0.12f, 0.25f), H * 1.0f, H / 22.0f, r.range(0.15f, 0.35f), 60000, seed + 2);
}

/** Zen garden — raked sand rippling around a few stones. */
void piece_zen(Canvas& c, int X, int Y, int W, int H, uint32_t seed) {
  Rng r(seed);
  struct Stone { float x, y, r; };
  Stone stones[3];
  const int n = 2 + static_cast<int>(r.next() * 2);
  for (int i = 0; i < n; ++i) stones[i] = {W * r.range(0.2f, 0.8f), H * r.range(0.2f, 0.8f), r.range(26, 52)};
  constexpr float kPitch = 11.0f;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      float best = 1e9f;
      int bi = 0;
      for (int i = 0; i < n; ++i) {
        const float dx = x - stones[i].x, dy = (y - stones[i].y) * 1.15f;
        const float d = std::sqrt(dx * dx + dy * dy) - stones[i].r;
        if (d < best) {
          best = d;
          bi = i;
        }
      }
      bool ink;
      if (best < 0) {
        // Stone: lit from the upper left, crisp dithered body.
        const float dx = (x - stones[bi].x) / stones[bi].r, dy = (y - stones[bi].y) / stones[bi].r;
        const float lit = clamp01(0.5f - 0.45f * dx - 0.45f * dy);
        ink = static_cast<float>(hash2(x, y, seed) & 255) / 255.0f > lit * 0.75f;
      } else if (best < 7.0f * kPitch) {
        ink = std::fmod(best, kPitch) < 2.2f;  // ripples around the stone
      } else {
        const float wave = 3.0f * std::sin(x * 0.02f + y * 0.002f);
        ink = std::fmod(y + wave + 400.0f, kPitch) < 2.2f;  // straight rake lines
      }
      if (ink) c.set_pixel(X + x, Y + y, Gray::G0);
    }
  }
}

/** Galaxy — a dithered spiral galaxy on a dark field of stars. */
void piece_galaxy(Canvas& c, int X, int Y, int W, int H, uint32_t seed) {
  Rng r(seed);
  const float cx = W * r.range(0.42f, 0.58f), cy = H * r.range(0.42f, 0.55f);
  const float tilt = r.range(0.45f, 0.65f), rot = r.range(0, 2 * kPi);
  const float R = W * 0.62f;
  diffuse(c, X, Y, W, H, [&](int x, int y) {
    float dx = x - cx, dy = (y - cy) / tilt;
    const float cr = std::cos(rot), sr = std::sin(rot);
    const float ux = dx * cr - dy * sr, uy = dx * sr + dy * cr;
    const float rr = std::sqrt(ux * ux + uy * uy) + 1.0f;
    const float th = std::atan2(uy, ux);
    const float arm = 0.5f + 0.5f * std::cos(2.0f * (th - 2.2f * std::log(rr / 18.0f)));
    const float core = std::exp(-rr / (R * 0.09f));
    const float a3 = arm * arm * arm;
    const float disk = std::exp(-rr / (R * 0.42f)) * (0.06f + 0.94f * a3);
    const float dust = 0.85f + 0.3f * value_noise(x * 0.03f, y * 0.03f, seed);
    const float light = clamp01((1.6f * core + 1.1f * disk) * dust);
    if ((hash2(x, y, seed + 9) & 0x7FF) == 0) return 0.0f;  // stars
    return 0.97f - 0.95f * light;
  });
}

/** Night city — a skyline of lit windows, moon, and their reflection on the river. */
void piece_city(Canvas& c, int X, int Y, int W, int H, uint32_t seed) {
  Rng r(seed);
  const int water = static_cast<int>(H * 0.74f);
  diffuse(c, X, Y, W, water, [&](int, int y) { return 0.97f - 0.35f * (static_cast<float>(y) / water); });
  // Moon.
  const int mx = static_cast<int>(W * r.range(0.65f, 0.82f)), my = static_cast<int>(H * 0.14f);
  c.fill_disc(X + mx, Y + my, 30, Gray::G3);
  c.fill_disc(X + mx + 11, Y + my - 6, 26, Gray::G0);
  // Skyline (solid ink) with lit windows (paper).
  std::vector<uint8_t> lit;
  int x = 0;
  while (x < W) {
    const int bw = static_cast<int>(r.range(28, 70));
    const int bh = static_cast<int>(r.range(H * 0.12f, H * 0.48f));
    const int top = water - bh;
    c.fill_rect(X + x, Y + top, std::min(bw, W - x) - 3, bh, Gray::G0);
    if (r.next() < 0.3f) c.fill_rect(X + x + bw / 2 - 2, Y + top - 22, 4, 22, Gray::G0);  // antenna
    for (int wy = top + 10; wy < water - 10; wy += 14) {
      for (int wx = x + 6; wx + 6 < x + bw - 6; wx += 11) {
        if (r.next() < 0.36f) c.fill_rect(X + wx, Y + wy, 5, 7, Gray::G3);
      }
    }
    x += bw;
  }
  // River: dark water, broken streaks under bright windows and the moon.
  for (int y = water; y < H; ++y) {
    const int sy = water - (y - water) - 1;  // mirrored row
    const float depth = static_cast<float>(y - water) / (H - water);
    for (int xx = 0; xx < W; ++xx) {
      const bool bright = sy >= 0 && c.get_pixel(X + xx, Y + sy) == Gray::G3 && sy < water - 4;
      const bool ripple = ((y + static_cast<int>(6 * std::sin(xx * 0.05f + y * 0.4f))) % 6) < 2;
      const bool stars = (hash2(xx, y, seed) & 0x3F) == 0 && depth > 0.2f;
      c.set_pixel(X + xx, Y + y, (bright && ripple && (hash2(xx, y, seed + 3) & 3) != 0) || stars ? Gray::G3 : Gray::G0);
    }
  }
}

/** Labyrinth — a generated maze, entrance top-left, exit bottom-right. */
void piece_maze(Canvas& c, int X, int Y, int W, int H, uint32_t seed) {
  Rng r(seed);
  const int cell = 30;
  const int cols = (W - 24) / cell, rows = (H - 24) / cell;
  const int ox = X + (W - cols * cell) / 2, oy = Y + (H - rows * cell) / 2;
  // walls bit: 1 = right wall, 2 = bottom wall.
  std::vector<uint8_t> walls(static_cast<size_t>(cols * rows), 3);
  std::vector<uint8_t> seen(static_cast<size_t>(cols * rows), 0);
  std::vector<int> stack;
  stack.push_back(0);
  seen[0] = 1;
  while (!stack.empty()) {
    const int cur = stack.back();
    const int cx = cur % cols, cy = cur / cols;
    int opts[4], n = 0;
    if (cx > 0 && !seen[static_cast<size_t>(cur - 1)]) opts[n++] = cur - 1;
    if (cx < cols - 1 && !seen[static_cast<size_t>(cur + 1)]) opts[n++] = cur + 1;
    if (cy > 0 && !seen[static_cast<size_t>(cur - cols)]) opts[n++] = cur - cols;
    if (cy < rows - 1 && !seen[static_cast<size_t>(cur + cols)]) opts[n++] = cur + cols;
    if (n == 0) {
      stack.pop_back();
      continue;
    }
    const int nxt = opts[static_cast<int>(r.next() * n) % n];
    if (nxt == cur + 1) walls[static_cast<size_t>(cur)] &= ~1;
    else if (nxt == cur - 1) walls[static_cast<size_t>(nxt)] &= ~1;
    else if (nxt == cur + cols) walls[static_cast<size_t>(cur)] &= ~2;
    else walls[static_cast<size_t>(nxt)] &= ~2;
    seen[static_cast<size_t>(nxt)] = 1;
    stack.push_back(nxt);
  }
  constexpr int kT = 4;
  c.fill_rect(ox, oy, cols * cell + kT, kT, Gray::G0);  // top
  c.fill_rect(ox, oy, kT, rows * cell + kT, Gray::G0);   // left
  for (int yy = 0; yy < rows; ++yy) {
    for (int xx = 0; xx < cols; ++xx) {
      const uint8_t w = walls[static_cast<size_t>(yy * cols + xx)];
      const int px = ox + xx * cell, py = oy + yy * cell;
      if (w & 1) c.fill_rect(px + cell, py, kT, cell + kT, Gray::G0);
      if (w & 2) c.fill_rect(px, py + cell, cell + kT, kT, Gray::G0);
    }
  }
  // Openings + markers.
  c.fill_rect(ox, oy + kT, kT, cell - kT, Gray::G3);
  c.fill_rect(ox + cols * cell, oy + (rows - 1) * cell + kT, kT, cell - kT, Gray::G3);
  c.fill_disc(ox + cell / 2 + 2, oy + cell / 2 + 2, 6, Gray::G0);
  c.stroke_circle(ox + (cols - 1) * cell + cell / 2 + 2, oy + (rows - 1) * cell + cell / 2 + 2, 8, Gray::G0, 3);
}

}  // namespace

const char* screensaver_name(int index) {
  static const char* kNames[kScreensaverCount] = {"Ranges",     "Contours", "Moonrise", "Fir & fog",
                                                  "Currents",   "Halftone sun", "Tide lines", "Fern",
                                                  "Zen garden", "Galaxy",   "Night city", "Labyrinth"};
  if (index < 0 || index >= kScreensaverCount) return "Plain";
  return kNames[index];
}

void draw_screensaver(Canvas& c, int index, int x, int y, int w, int h, uint32_t seed) {
  c.fill_rect(x, y, w, h, Gray::G3);
  switch (index) {
    case 0:
      piece_ranges(c, x, y, w, h, seed);
      break;
    case 1:
      piece_contours(c, x, y, w, h, seed);
      break;
    case 2:
      piece_moon(c, x, y, w, h, seed);
      break;
    case 3:
      piece_pines(c, x, y, w, h, seed);
      break;
    case 4:
      piece_flow(c, x, y, w, h, seed);
      break;
    case 5:
      piece_halftone(c, x, y, w, h, seed);
      break;
    case 6:
      piece_tide(c, x, y, w, h, seed);
      break;
    case 7:
      piece_fern(c, x, y, w, h, seed);
      break;
    case 8:
      piece_zen(c, x, y, w, h, seed);
      break;
    case 9:
      piece_galaxy(c, x, y, w, h, seed);
      break;
    case 10:
      piece_city(c, x, y, w, h, seed);
      break;
    default:
      piece_maze(c, x, y, w, h, seed);
      break;
  }
}

}  // namespace pocket
