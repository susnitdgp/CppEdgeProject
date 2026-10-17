#pragma once
// config/config.json -> Config. Every field is required; a missing one fails at start-up.
#include <simdjson.h>

#include <cstdint>
#include <string>

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
    std::int64_t id, segment_code, lots;
    double tick_size, protect_points;   // orders go out as marketable limits: ref price +/- protect_points
  } instrument;
  struct Candle {
    std::int64_t timeframe_minutes, close_grace_ms;
  } candle;
  struct Strategy {
    std::string name;
    std::int64_t fast, slow;
  } strategy;
  struct Redis {
    std::string host, password_env, md_app_key, md_secret_key, ia_token, ia_user_id, state_prefix;
    std::int64_t port, db;
  } redis;
  struct Logging {
    std::string dir, level;
  } logging;
};

inline Config load_config(const std::string& path) {
  auto json = simdjson::padded_string::load(path);
  simdjson::ondemand::parser parser;
  auto doc = parser.iterate(json);
  auto str = [](auto obj, const char* key) { return std::string(std::string_view(obj[key])); };
  Config c;
  {
    auto o = doc["xts"].get_object();
    c.xts = {str(o, "host"), str(o, "client_id"), bool(o["live_orders_enabled"])};
  }
  {
    auto o = doc["instrument"].get_object();
    c.instrument = {str(o, "symbol"), str(o, "segment"), str(o, "product"),
                    int64_t(o["exchange_instrument_id"]), int64_t(o["segment_code"]), int64_t(o["lots"]),
                    double(o["tick_size"]), double(o["protect_points"])};
  }
  {
    auto o = doc["candle"].get_object();
    c.candle = {int64_t(o["timeframe_minutes"]), int64_t(o["close_grace_ms"])};
  }
  {
    auto o = doc["strategy"].get_object();
    c.strategy = {str(o, "name"), int64_t(o["fast"]), int64_t(o["slow"])};
  }
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
  return c;
}

}  // namespace edge
