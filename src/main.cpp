// cpp_edge: five-thread XTS trading bot.
//   T1 market data socket --Tick--> T2 candles --Candle--> T3 strategy --Signal--+
//                                                                                v
//   T5 interactive socket ------------------------OrderUpdate----------------> T4 order engine
// main logs in to Market Data once (T1's socket and T3's history calls share the session) and
// reads today's Interactive session from Redis (T4 orders, T5 socket).
// Usage: cpp_edge [config/config.json]   (load the Redis password first: see run.sh)
#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <format>
#include <string>
#include <thread>

#include "edge/core/config.hpp"
#include "edge/core/log.hpp"
#include "edge/core/types.hpp"
#include "edge/net/rest_client.hpp"
#include "edge/store/redis_store.hpp"
#include "edge/threads/t1_market_data.hpp"
#include "edge/threads/t2_candle_builder.hpp"
#include "edge/threads/t3_strategy_runner.hpp"
#include "edge/threads/t4_order_engine.hpp"
#include "edge/threads/t5_order_updates.hpp"
#include "edge/xts/session.hpp"

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
    XtsSession md;
    {
      RestClient http(cfg.xts.md_base(), cfg.engine.rest);
      md = md_login(http, redis, cfg.redis);
    }
    ELOG_INFO("market data session up for {}", md.user_id);
    const auto saved = redis.get_field(std::format("position:{}", cfg.instrument.id), "lots");
    const auto lots = saved.empty() ? 0 : std::stoll(saved);
    const std::int32_t start_position = (lots > 0) - (lots < 0);

    Queue<Tick> ticks;
    Queue<Candle> candles;
    Queue<EngineEvent> engine;
    {
      // Consumers start first; jthreads stop and join in reverse order, so producers stop first.
      std::jthread t4{OrderEngineThread{cfg, ia, engine}};
      std::jthread t5{OrderUpdateThread{cfg, ia, engine}};
      std::jthread t3{StrategyThread{cfg, md, start_position, candles, engine}};
      std::jthread t2{CandleThread{cfg, ticks, candles}};
      std::jthread t1{MarketDataThread{cfg, md, ticks}};
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
