#pragma once
// Thread 3: Candle queue -> Strategy -> Signal (target position) on the engine queue.
// A strategy returns a new target (+1 long, -1 short, 0 flat) only when it changes.
// EmaCross is a placeholder to exercise the pipeline; it starts cold (no history warm-up yet).
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>

#include "edge/config.hpp"
#include "edge/log.hpp"
#include "edge/types.hpp"

namespace edge {

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual std::optional<std::int32_t> on_candle(const Candle& c) = 0;
};

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

inline std::unique_ptr<Strategy> make_strategy(const Config::Strategy& s) {
  if (s.name == "ema_cross") return std::make_unique<EmaCross>(s.fast, s.slow);
  throw std::runtime_error("unknown strategy: " + s.name);
}

class StrategyThread {
 public:
  StrategyThread(const Config& cfg, Queue<Candle>& in, Queue<EngineEvent>& out)
      : strategy_(make_strategy(cfg.strategy)), in_(in), out_(out) {}

  void operator()(std::stop_token st) {
    set_thread_name("edge-strategy");
    Candle c;
    while (!st.stop_requested()) {
      if (!in_.wait_dequeue_timed(c, kPollTimeout)) continue;
      if (const auto target = strategy_->on_candle(c)) {
        ELOG_INFO("signal: target {} at {} (bar {})", *target, c.close, c.open_ts_ms);
        out_.enqueue(Signal{c.instrument_id, *target, c.close, c.open_ts_ms});
      }
    }
  }

 private:
  std::unique_ptr<Strategy> strategy_;
  Queue<Candle>& in_;
  Queue<EngineEvent>& out_;
};

}  // namespace edge
