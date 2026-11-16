#pragma once
/**
 * Pocket UI kit — the one place screens get their visual language from.
 *
 * Everything is drawn for the 1-bit panel on the shared layout grid
 * (grayscale.hpp): rows are kFocusRowH tall on a kRowPitch rhythm, headers sit at
 * kContentTop, bottom actions at kBottomCtaY. Keeping that grid means focus /
 * partial-refresh dirty rects in App stay valid whatever the styling.
 */
#include "pocket/canvas.hpp"
#include <cstdint>
#include <string_view>

namespace pocket {
namespace ui {

/** Vector icon set — single stroke weight, drawn on a 24-unit grid. */
enum class Icon : uint8_t {
  Notes,
  Ledger,
  Clock,
  Pass,
  Weather,
  Music,
  Reading,
  Settings,
  Update,
  ChevronRight,
  ChevronLeft,
  Check,
  Play,
  Pause,
  Stop,
  Plus,
  Mic,
  Wifi,
  Lock,
  Cloud,
  Trash,
  Alarm,
  Timer,
  Sd,
  Info,
  Phone,
  Rain,
  Sound,
  Display,
  Home,
};

/** Draw `icon` centered on (cx, cy) in a `size`×`size` box. */
void icon(Canvas& c, Icon icon, int cx, int cy, int size, Gray g);

/**
 * Screen header: serif title at kContentTop. `back` prefixes a chevron that says
 * "side button goes back". Returns the y where content should start (kListTop).
 */
int header(Canvas& c, std::string_view title, bool back = false);

/** Small letter-spaced section label (e.g. "PREFERENCES") with a hairline. */
void section(Canvas& c, int y, std::string_view label);

enum class RowKind : uint8_t {
  Plain,   // label only
  Nav,     // label + chevron (opens a page)
  Value,   // label + right-aligned value (cycles on select)
  Toggle,  // label + switch
  Check,   // checkbox + label (strike when on)
  Radio,   // radio + label
  Back,    // chevron-left + "Back", quieter
  Action,  // label in strong weight (a command)
};

struct Row {
  std::string_view label;
  RowKind kind = RowKind::Plain;
  std::string_view value = {};
  bool on = false;
  /** Optional leading icon (Icon::Home = none). */
  Icon lead = Icon::Home;
  bool has_lead = false;
};

/** One list row at the standard geometry (x = kSideMargin, w = kFocusRowW, h = kFocusRowH). */
void row(Canvas& c, int y, const Row& r, bool focused);

/** Shorthand: parses "Label: Value" → Value/Toggle (On/Off), "Back" → Back, trailing "…" → Nav. */
void auto_row(Canvas& c, int y, std::string_view text, bool focused);

/** Hairline between rows (drawn in the gap under row `y`). */
void row_rule(Canvas& c, int y);

/** Segmented control. Display-only (tabs switch via Up/Down on their screens). */
void tabs(Canvas& c, int y, const char* const* labels, int n, int active);

/** Bottom (or inline) button: filled when focused, outlined otherwise. Centered label. */
void button(Canvas& c, int y, std::string_view label, bool focused, Icon lead = Icon::Home,
            bool has_lead = false);

/** Centered empty state: icon in a ring, title, wrapped body. */
void empty_state(Canvas& c, Icon icon, std::string_view title, std::string_view body, int cy = 300);

/** Onboarding progress: "Step n of total" + segmented bar. */
void steps(Canvas& c, int step, int total);

/** Footer meta line on a dotted rule (offline hints, timestamps). */
void footer(Canvas& c, std::string_view text);

/** Inverted toast just above the footer for transient errors. */
void toast(Canvas& c, std::string_view msg);

/**
 * First visible index of a list window so `focus` stays on screen with one row of
 * look-ahead. Pure function of (focus, count, visible) — no scroll state to desync.
 */
int window_start(int focus, int count, int visible);

/** Slim scrollbar on the right gutter for a list window (no-op when it all fits). */
void scrollbar(Canvas& c, int top, int height, int start, int visible, int count);

/** Key/value line for detail pages: muted key left, value right. Returns next y. */
int kv(Canvas& c, int y, std::string_view key, std::string_view value);

}  // namespace ui
}  // namespace pocket
