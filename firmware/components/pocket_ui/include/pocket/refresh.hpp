#pragma once
#include <cstdint>

namespace pocket {

enum class RefreshMode : uint8_t { Partial, Full };

/**
 * Ghosting mitigation: after N consecutive partials → force full (Spec §6).
 * Tiny status/PIN region updates count less so the bar/clock don't force a full flash.
 */
class RefreshPolicy {
 public:
  static constexpr int kGhostingN = 16;
  /** Settings → Display → "Deep refresh" picks this tighter budget. */
  static constexpr int kGhostingNFrequent = 8;
  /** Weight of a whole-screen partial (screen change) — it leaves far more residue than a row. */
  static constexpr int kScreenChangeWeight = 2;
  /** Ghost debt that earns a clean full refresh once the user pauses (Kindle-style). */
  static constexpr int kIdleCleanDebt = 2;
  static constexpr int kIdleCleanDebtFrequent = 1;

  RefreshMode plan(bool wants_partial);
  void on_screen_enter_full();
  void on_applied(RefreshMode mode, int weight = 1);
  /** Status bar / digit region — counts as half a partial toward ghosting. */
  void on_applied_tiny(RefreshMode mode);
  /** Content-band repaint (focus browse / page turn): half a partial toward the forced budget
   * (never a surprise flash mid-browse) but full weight toward the idle clean-up. */
  void on_applied_band();
  int partial_count() const { return partial_count_; }
  /** Residue from non-trivial partials since the last full refresh (tiny ticks excluded). */
  int clean_debt() const { return clean_debt_; }
  /** User-tunable ghosting budget; defaults to kGhostingN until set otherwise. */
  void set_ghosting_budget(int n) { ghosting_n_ = n; }

 private:
  int partial_count_ = 0;
  int tiny_credit_ = 0;
  int clean_debt_ = 0;
  int ghosting_n_ = kGhostingN;
};

}  // namespace pocket