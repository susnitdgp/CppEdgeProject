#pragma once
// Two JSON files -> Config. Every field is required; a missing one fails at start-up.
//   config.json         what to trade and where: XTS, instrument, candle timeframe, strategy, Redis, logging;
//                       it names the strategy's own JSON and the session file (paths relative to it)
//   engine-config.json  how the engine runs: queue polling, candle close grace, order-engine
//                       reconciliation, REST connection tuning, socket reconnect/watchdog, recorder.
//                       Read from the same folder as config.json.
#include <simdjson.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace edge {

struct Config {
  struct Xts {
    std::string host, client_id;
    bool live_orders_enabled;
    std::string md_base() const { return "https://" + host + "/apimarketdata"; }
    std::string ia_base() const { return "https://" + host + "/interactive"; }
  } xts;
  struct Instrument {
    std::string symbol, segment, product;
    std::int64_t id, segment_code, lots, qty_per_lot;   // qty_per_lot: XTS quantity per lot (MCX 1, NIFTY 65)
    double tick_size, protect_points;   // orders go out as marketable limits: ref price +/- protect_points
  } instrument;
  struct Candle {
    std::int64_t timeframe_minutes;
  } candle;
  struct Strategy {
    std::string name, params_file;  // params_file: the strategy's own JSON (indicator, trade, history_days)
  } strategy;
  std::string session_file;         // exchange calendar + entry/square-off rules
  struct Redis {
    std::string host, password_env, md_app_key, md_secret_key, ia_token, ia_user_id, state_prefix;
    std::int64_t port, db;
  } redis;
  struct Logging {
    std::string dir, level;
  } logging;

  // ---- engine-config.json
  struct Engine {
    std::int64_t queue_poll_ms;           // consumer wake-up to check for shutdown
    struct Candle {
      std::int64_t close_grace_ms;        // close a quiet bar this long after its end
    } candle;
    struct OrderEngine {
      std::int64_t reconcile_interval_ms; // broker position check (also keeps REST warm)
      std::int64_t stale_order_ms;        // warn when a working order has no final update
    } order_engine;
    struct Rest {
      std::int64_t connect_timeout_ms, request_timeout_ms, tcp_keepidle_s, tcp_keepintvl_s, max_conn_age_s,
          dns_cache_s;
    } rest;
    struct Socket {
      std::int64_t reconnect_min_ms, reconnect_max_ms, watchdog_tick_ms;
    } socket;
    struct Recorder {
      bool enabled;
      std::string dir;                  // <dir>/<symbol>/<YYYY-MM-DD>.jsonl.gz
      std::vector<std::int64_t> feeds;  // XTS message codes subscribed and recorded (1501, 1502, 1505, 1510)
      std::int64_t flush_interval_ms, max_queue;
    } recorder;
    std::chrono::milliseconds queue_poll() const { return std::chrono::milliseconds(queue_poll_ms); }
  } engine;
};

inline Config load_config(const std::string& path) {
  auto str = [](auto obj, const char* key) { return std::string(std::string_view(obj[key])); };
  const auto dir = std::filesystem::path(path).parent_path();
  auto resolve = [&dir](const std::string& file) { return (dir / file).string(); };  // relative to config.json
  Config c;
  {
    auto json = simdjson::padded_string::load(path);
    simdjson::ondemand::parser parser;
    auto doc = parser.iterate(json);
    {
      auto o = doc["xts"].get_object();
      c.xts = {str(o, "host"), str(o, "client_id"), bool(o["live_orders_enabled"])};
    }
    {
      auto o = doc["instrument"].get_object();
      c.instrument = {str(o, "symbol"), str(o, "segment"), str(o, "product"),
                      int64_t(o["exchange_instrument_id"]), int64_t(o["segment_code"]), int64_t(o["lots"]),
                      int64_t(o["qty_per_lot"]),
                      double(o["tick_size"]), double(o["protect_points"])};
    }
    c.candle = {int64_t(doc["candle"]["timeframe_minutes"])};
    {
      auto o = doc["strategy"].get_object();
      c.strategy = {str(o, "name"), resolve(str(o, "params_file"))};
    }
    c.session_file = resolve(std::string(std::string_view(doc["session_file"])));
    {
      auto o = doc["redis"].get_object();
      c.redis = {str(o, "host"), str(o, "password_env"), str(o, "md_app_key"), str(o, "md_secret_key"),
                 str(o, "ia_token"), str(o, "ia_user_id"), str(o, "state_prefix"),
                 int64_t(o["port"]), int64_t(o["db"])};
    }
    {
      auto o = doc["logging"].get_object();
      c.logging = {str(o, "dir"), str(o, "level")};
    }
  }
  {
    const auto engine_path = resolve("engine-config.json");
    auto json = simdjson::padded_string::load(engine_path);
    simdjson::ondemand::parser parser;
    auto doc = parser.iterate(json);
    auto& e = c.engine;
    e.queue_poll_ms = int64_t(doc["queue_poll_ms"]);
    e.candle = {int64_t(doc["candle"]["close_grace_ms"])};
    {
      auto o = doc["order_engine"].get_object();
      e.order_engine = {int64_t(o["reconcile_interval_ms"]), int64_t(o["stale_order_ms"])};
    }
    {
      auto o = doc["rest"].get_object();
      e.rest = {int64_t(o["connect_timeout_ms"]), int64_t(o["request_timeout_ms"]), int64_t(o["tcp_keepidle_s"]),
                int64_t(o["tcp_keepintvl_s"]), int64_t(o["max_conn_age_s"]), int64_t(o["dns_cache_s"])};
    }
    {
      auto o = doc["socket"].get_object();
      e.socket = {int64_t(o["reconnect_min_ms"]), int64_t(o["reconnect_max_ms"]), int64_t(o["watchdog_tick_ms"])};
    }
    {
      auto o = doc["recorder"].get_object();
      e.recorder.enabled = bool(o["enabled"]);
      e.recorder.dir = str(o, "dir");
      for (auto v : o["feeds"].get_array()) e.recorder.feeds.push_back(int64_t(v));
      e.recorder.flush_interval_ms = int64_t(o["flush_interval_ms"]);
      e.recorder.max_queue = int64_t(o["max_queue"]);
    }
  }
  return c;
}

}  // namespace edge
