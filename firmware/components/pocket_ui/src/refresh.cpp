#include "pocket/refresh.hpp"

namespace pocket {

RefreshMode RefreshPolicy::plan(bool wants_partial) {
  if (!wants_partial) return RefreshMode::Full;
  if (partial_count_ >= ghosting_n_) return RefreshMode::Full;
  return RefreshMode::Partial;
}

void RefreshPolicy::on_screen_enter_full() {
  partial_count_ = 0;
  tiny_credit_ = 0;
  clean_debt_ = 0;
}

void RefreshPolicy::on_applied(RefreshMode mode, int weight) {
  if (mode == RefreshMode::Full) {
    partial_count_ = 0;
    tiny_credit_ = 0;
    clean_debt_ = 0;
  } else {
    partial_count_ += weight;
    clean_debt_ += weight;
  }
}

void RefreshPolicy::on_applied_band() {
  on_applied_tiny(RefreshMode::Partial);
  ++clean_debt_;
}

void RefreshPolicy::on_applied_tiny(RefreshMode mode) {
  if (mode == RefreshMode::Full) {
    partial_count_ = 0;
    tiny_credit_ = 0;
    clean_debt_ = 0;
    return;
  }
  // Two tiny region refreshes ≈ one partial toward ghosting budget.
  ++tiny_credit_;
  if (tiny_credit_ >= 2) {
    tiny_credit_ = 0;
    ++partial_count_;
  }
}

}  // namespace pocket
