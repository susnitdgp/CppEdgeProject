// cpp_edge: five-thread XTS trading bot.
//   T1 market data socket --Tick--> T2 candles --Candle--> T3 strategy --Signal--+
//                                                                                v
//   T5 interactive socket ------------------------OrderUpdate----------------> T4 order engine
// Usage: cpp_edge [config/config.json]   (load the Redis password first: see AGENT.md)
#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

#include "edge/candle_thread.hpp"
#include "edge/config.hpp"
#include "edge/log.hpp"
#include "edge/market_data_thread.hpp"
#include "edge/order_engine_thread.hpp"
#include "edge/order_update_thread.hpp"
#include "edge/strategy_thread.hpp"
#include "edge/types.hpp"
#include "edge/xts.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }
}  // namespace

int main(int argc, char** argv) {
  using namespace edge;
  using namespace std::chrono_literals;

  Config cfg;
  try {
    cfg = load_config(argc > 1 ? argv[1] : "config/config.json");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "config: %s\n", e.what());
    return 1;
  }
  log::init(cfg.logging.dir, cfg.logging.level);
  ELOG_INFO("cpp_edge {} starting: {} ({}) {} lot(s), {}m candles, strategy {}, live orders {}", EDGE_VERSION,
            cfg.instrument.symbol, cfg.instrument.id, cfg.instrument.lots, cfg.candle.timeframe_minutes,
            cfg.strategy.name, cfg.xts.live_orders_enabled);

  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  int rc = 0;
  try {
    RedisStore redis(cfg.redis);
    const auto ia = ia_session(redis, cfg.redis);
    Queue<Tick> ticks;
    Queue<Candle> candles;
    Queue<EngineEvent> engine;
    {
      // Consumers start first; jthreads stop and join in reverse order, so producers stop first.
      std::jthread t4{OrderEngineThread{cfg, ia, engine}};
      std::jthread t5{OrderUpdateThread{cfg, ia, engine}};
      std::jthread t3{StrategyThread{cfg, candles, engine}};
      std::jthread t2{CandleThread{cfg, ticks, candles}};
      std::jthread t1{MarketDataThread{cfg, ticks}};
      while (!g_stop.load()) std::this_thread::sleep_for(200ms);
      ELOG_INFO("shutdown requested");
    }
  } catch (const std::exception& e) {
    ELOG_ERROR("fatal: {}", e.what());
    rc = 1;
  }
  curl_global_cleanup();
  ELOG_INFO("cpp_edge stopped");
  return rc;
}
