#pragma once
// Thread 3: Candle queue -> Strategy (edge/strategy/) -> Signal (target position) on the engine queue.
#include <chrono>
#include <memory>
#include <stop_token>

#include "edge/core/config.hpp"
#include "edge/core/log.hpp"
#include "edge/core/types.hpp"
#include "edge/strategy/factory.hpp"

namespace edge {

class StrategyThread {
 public:
  StrategyThread(const Config& cfg, Queue<Candle>& in, Queue<EngineEvent>& out)
      : strategy_(make_strategy(cfg.strategy)), poll_(cfg.engine.queue_poll()), in_(in), out_(out) {}

  void operator()(std::stop_token st) {
    set_thread_name("edge-strategy");
    Candle c;
    while (!st.stop_requested()) {
      if (!in_.wait_dequeue_timed(c, poll_)) continue;
      if (const auto target = strategy_->on_candle(c)) {
        ELOG_INFO("signal: target {} at {} (bar {})", *target, c.close, c.open_ts_ms);
        out_.enqueue(Signal{c.instrument_id, *target, c.close, c.open_ts_ms});
      }
    }
  }

 private:
  std::unique_ptr<Strategy> strategy_;
  std::chrono::milliseconds poll_;
  Queue<Candle>& in_;
  Queue<EngineEvent>& out_;
};

}  // namespace edge
