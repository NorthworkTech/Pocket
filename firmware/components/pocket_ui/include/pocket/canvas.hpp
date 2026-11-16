#pragma once
#include "pocket/grayscale.hpp"
#include <cstdint>
#include <cstring>
#include <string_view>

namespace pocket {

/** Logical framebuffer 480×800, 2 bits/pixel packed (4 pixels per byte). */
class Canvas {
 public:
  void clear(Gray g = Gray::G3);
  void set_pixel(int x, int y, Gray g);
  Gray get_pixel(int x, int y) const;

  void fill_rect(int x, int y, int w, int h, Gray g);
  void stroke_rect(int x, int y, int w, int h, Gray g);
  /** Soft rounded fill for tiles (e-ink). `r` is corner radius in pixels. */
  void fill_round_rect(int x, int y, int w, int h, int r, Gray g);
  /** Rounded outline; `thickness` ≥ 1 draws a soft border without a harsh box.
   * `bg` is the color used to clear the interior — must match the surface the
   * outline sits on (defaults to paper/G3; pass G0 for a white outline drawn
   * over a filled-black focus tile, or the glyph degrades into a solid blob). */
  void stroke_round_rect(int x, int y, int w, int h, int r, Gray g, int thickness = 1,
                         Gray bg = Gray::G3);
  void hline(int x, int y, int w, Gray g);
  void vline(int x, int y, int h, Gray g);
  void line(int x0, int y0, int x1, int y1, Gray g);

  /** Type roles — DM Sans (UI) + Source Serif 4 (display), 1-bit for the mono panel. */
  enum class TextRole : uint8_t {
    StatusBar,    // DM Sans SemiBold 21
    Secondary,    // DM Sans 23
    Body,         // DM Sans 27
    ScreenTitle,  // Source Serif 4 SemiBold 40
    WordMark,     // Source Serif 4 SemiBold 40
    PinDigit,     // Source Serif 4 Display 48 (digits)
    HugeClock,    // Source Serif 4 Display 170 (digits + colon)
    Caption,      // DM Sans Medium 19 — labels, meta
    BodyStrong,   // DM Sans SemiBold 27 — emphasis
    Hero,         // Source Serif 4 Display 92 (digits, °, AM/PM)
  };

  void draw_text(int x, int y, std::string_view text, TextRole role, Gray g);
  void draw_text_centered(int cx, int y, std::string_view text, TextRole role, Gray g);
  void draw_text_fit(int x, int y, int max_w, std::string_view text, TextRole role, Gray g);
  /** Word-wrap into max_w; returns y just below the last line (for stacking). */
  int draw_text_wrapped(int x, int y, int max_w, int line_gap, std::string_view text, TextRole role,
                        Gray g);
  int text_width(std::string_view text, TextRole role) const;
  int text_height(TextRole role) const;

  /** Focused list row: black pill, white label at the same inset as unfocused rows. */
  void draw_focus_tile(int x, int y, int w, int h, std::string_view label, TextRole role);
  /** Dotted 1px rule — reads as a light hairline on the 1-bit panel. */
  void dotted_hline(int x, int y, int w, Gray g, int step = 3);
  /** Fill with a 1-bit ordered-dither tone, 0 = paper … 16 = solid ink. */
  void fill_tone(int x, int y, int w, int h, int level);
  void fill_disc(int cx, int cy, int r, Gray g);
  void stroke_circle(int cx, int cy, int r, Gray g, int thickness = 1);
  /** Thick line with round caps. */
  void thick_line(int x0, int y0, int x1, int y1, int thickness, Gray g);
  bool draw_qr(int x, int y, int size, std::string_view payload);

  const uint8_t* pixels() const { return buf_; }
  static constexpr size_t kBufBytes = (kCanvasW * kCanvasH) / 4;

 private:
  uint8_t buf_[kBufBytes]{};
};

}  // namespace pocket
