#pragma once
// Thread 3: closed bars -> Strategy -> TradeRules -> Signal (target position) on the engine queue.
// Warm-up: at start, history_days (strategy JSON) of XTS history are fed to the strategy, indicator
//   only (no trades), so live crosses match a chart that has history behind it.
// Gap fill: when a live bar arrives more than one bar after the last one seen (the partial first bar
//   the candle thread skips, a socket outage), the missing bars are fetched from XTS history and fed
//   first, indicator only.
// Trading: a live closed bar is turned into a target position by TradeRules (long/short switches,
//   session entry window, square-off, new day) and sent to the order engine when it changes.
// start_position: the position held at start (sign of Redis lots), so exits still work after a restart.
#include <cstdint>
#include <stop_token>

#include "edge/core/config.hpp"
#include "edge/core/log.hpp"
#include "edge/core/types.hpp"
#include "edge/net/rest_client.hpp"
#include "edge/strategy/factory.hpp"
#include "edge/strategy/session.hpp"
#include "edge/strategy/trade_rules.hpp"
#include "edge/xts/history.hpp"
#include "edge/xts/session.hpp"

namespace edge {

class StrategyThread {
 public:
  StrategyThread(const Config& cfg, XtsSession md, std::int32_t start_position, Queue<Candle>& in,
                 Queue<EngineEvent>& out)
      : cfg_(cfg), md_(std::move(md)), setup_(load_strategy(cfg)),
        rules_(Session::load(cfg.session_file), setup_.allow_long, setup_.allow_short),
        tf_ms_(cfg.candle.timeframe_minutes * 60'000), current_(start_position), in_(in), out_(out) {}

  void operator()(std::stop_token st) {
    set_thread_name("edge-strategy");
    try {
      RestClient http(cfg_.xts.md_base(), cfg_.engine.rest);
      http.set_token(md_.token);
      const auto now = now_ms();
      const auto n = feed_history(http, now - setup_.history_days * 86'400'000, now);
      ELOG_INFO("strategy {} warmed up on {} bars ({} days), ready {}: {}", cfg_.strategy.name, n,
                setup_.history_days, setup_.strategy->ready(), setup_.strategy->state());
      ELOG_INFO("trading long {}, short {}; position at start {}", setup_.allow_long, setup_.allow_short, current_);
      Candle c;
      while (!st.stop_requested()) {
        if (!in_.wait_dequeue_timed(c, cfg_.engine.queue_poll())) continue;
        if (c.open_ts_ms <= last_ts_) continue;  // already fed from history
        if (last_ts_ > 0 && c.open_ts_ms > last_ts_ + tf_ms_) {
          if (const auto g = feed_history(http, last_ts_ + tf_ms_, c.open_ts_ms - 1); g > 0)
            ELOG_INFO("gap filled with {} history bars", g);
        }
        on_bar(c, true);
      }
    } catch (const std::exception& e) {
      ELOG_ERROR("strategy thread stopped: {}", e.what());
    }
  }

 private:
  std::size_t feed_history(RestClient& http, std::int64_t from_ms, std::int64_t to_ms) {
    std::size_t n = 0;
    try {
      for (const auto& b : fetch_history(http, cfg_, from_ms, to_ms)) {
        if (b.open_ts_ms <= last_ts_ || b.open_ts_ms > to_ms) continue;
        on_bar(b, false);
        ++n;
      }
    } catch (const std::exception& e) {
      ELOG_WARN("history fetch failed: {}", e.what());
    }
    return n;
  }

  void on_bar(const Candle& b, bool live) {
    last_ts_ = b.open_ts_ms;
    if (!rules_.in_session(b.open_ts_ms + tf_ms_)) {
      ELOG_INFO("off-session bar {} skipped (close {}, volume {})", b.open_ts_ms, b.close, b.volume);
      return;
    }
    const Cross x = setup_.strategy->on_candle(b);
    if (!live) return;
    ELOG_INFO("bar {} close {} cross {} {}", b.open_ts_ms, b.close, to_string(x), setup_.strategy->state());
    if (!setup_.strategy->ready()) return;
    if (const auto d = rules_.decide(x, b.open_ts_ms + tf_ms_, current_)) {
      current_ = d->target;
      ELOG_INFO("signal: target {} ({}) at {}", d->target, d->reason, b.close);
      out_.enqueue(Signal{b.instrument_id, d->target, b.close, b.open_ts_ms});
    }
  }

  const Config& cfg_;
  const XtsSession md_;
  StrategySetup setup_;
  TradeRules rules_;
  std::int64_t tf_ms_;
  std::int32_t current_;
  std::int64_t last_ts_ = 0;
  Queue<Candle>& in_;
  Queue<EngineEvent>& out_;
};

}  // namespace edge
