#pragma once
// Thread 2: Tick queue -> N-minute candles (config.json candle.timeframe_minutes) -> Candle queue.
// Buckets on exchange time (IST 09:00 is a multiple of 3 and 5 minutes, so Unix-ms flooring aligns).
// A bar closes on the first tick of the next bucket, or by wall clock after close_grace_ms
// (engine-config.json) when the market is quiet. The first bar after start-up is partial and is
// not emitted.
#include <algorithm>
#include <chrono>
#include <stop_token>
#include <utility>

#include "edge/core/config.hpp"
#include "edge/core/log.hpp"
#include "edge/core/types.hpp"

namespace edge {

class CandleThread {
 public:
  CandleThread(const Config& cfg, Queue<Tick>& in, Queue<Candle>& out)
      : tf_ms_(cfg.candle.timeframe_minutes * 60'000), grace_ms_(cfg.engine.candle.close_grace_ms),
        poll_(cfg.engine.queue_poll()), in_(in), out_(out) {}

  void operator()(std::stop_token st) {
    set_thread_name("edge-candle");
    Tick t;
    while (!st.stop_requested()) {
      if (in_.wait_dequeue_timed(t, poll_)) on_tick(t);
      if (open_ && now_ms() >= bar_.open_ts_ms + tf_ms_ + grace_ms_) close_bar();
    }
  }

 private:
  void on_tick(const Tick& t) {
    const auto bucket = t.exch_ts_ms - t.exch_ts_ms % tf_ms_;
    if (open_ && bucket > bar_.open_ts_ms) close_bar();
    if (open_ && bucket == bar_.open_ts_ms) {
      bar_.high = std::max(bar_.high, t.price);
      bar_.low = std::min(bar_.low, t.price);
      bar_.close = t.price;
      bar_.volume += t.qty;
      return;
    }
    if (bucket <= last_closed_ || (open_ && bucket < bar_.open_ts_ms)) {
      ELOG_DEBUG("late tick dropped: ts {} price {}", t.exch_ts_ms, t.price);
      return;
    }
    bar_ = {t.instrument_id, bucket, t.price, t.price, t.price, t.price, t.qty};
    open_ = true;
  }

  void close_bar() {
    open_ = false;
    last_closed_ = bar_.open_ts_ms;
    if (std::exchange(first_, false)) {
      ELOG_INFO("partial first bar {} skipped", bar_.open_ts_ms);
      return;
    }
    ELOG_INFO("bar {} O {} H {} L {} C {} V {}", bar_.open_ts_ms, bar_.open, bar_.high, bar_.low, bar_.close,
              bar_.volume);
    out_.enqueue(bar_);
  }

  const std::int64_t tf_ms_, grace_ms_;
  const std::chrono::milliseconds poll_;
  Queue<Tick>& in_;
  Queue<Candle>& out_;
  Candle bar_{};
  bool open_ = false, first_ = true;
  std::int64_t last_closed_ = 0;
};

}  // namespace edge
