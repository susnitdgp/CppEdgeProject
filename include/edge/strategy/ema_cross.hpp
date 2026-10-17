#pragma once
// EMA crossover: long while fast EMA > slow EMA, short otherwise. Placeholder to exercise the
// pipeline; it starts cold (no history warm-up yet) and stays silent for `slow` bars.
#include <cstdint>
#include <optional>

#include "edge/strategy/strategy.hpp"

namespace edge {

class EmaCross final : public Strategy {
 public:
  EmaCross(std::int64_t fast, std::int64_t slow)
      : a_fast_(2.0 / static_cast<double>(fast + 1)), a_slow_(2.0 / static_cast<double>(slow + 1)), warmup_(slow) {}

  std::optional<std::int32_t> on_candle(const Candle& c) override {
    if (n_++ == 0) fast_ = slow_ = c.close;
    fast_ += a_fast_ * (c.close - fast_);
    slow_ += a_slow_ * (c.close - slow_);
    if (n_ < warmup_) return std::nullopt;
    const std::int32_t target = fast_ > slow_ ? 1 : -1;
    if (target == last_) return std::nullopt;
    last_ = target;
    return target;
  }

 private:
  const double a_fast_, a_slow_;
  const std::int64_t warmup_;
  std::int64_t n_ = 0;
  double fast_ = 0, slow_ = 0;
  std::int32_t last_ = 0;
};

}  // namespace edge
