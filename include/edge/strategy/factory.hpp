#pragma once
// config.json strategy {name, params_file} -> Strategy + its trade switches + warm-up length.
// Each strategy lives in its own folder (strategy/<name>/<name>.hpp) and builds itself from the
// "indicator" section of its params file (from_json). The params file also holds
// "trade" {allow_long, allow_short} and "history_days" (XTS history fed at start-up).
// Adding a strategy: create strategy/<name>/<name>.hpp + config/strategies/<name>.json, and add one
// line to kStrategies below.
#include <simdjson.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "edge/core/config.hpp"
#include "edge/strategy/alpha_trend/alpha_trend.hpp"
#include "edge/strategy/obv_macd/obv_macd.hpp"
#include "edge/strategy/strategy.hpp"

namespace edge {

struct StrategySetup {
  std::unique_ptr<Strategy> strategy;
  bool allow_long = false, allow_short = false;
  std::int64_t history_days = 0;
};

inline constexpr struct {
  std::string_view name;
  std::unique_ptr<Strategy> (*make)(simdjson::dom::element indicator);
} kStrategies[] = {
    {"obv_macd", &ObvMacd::from_json},
    {"alpha_trend", &AlphaTrend::from_json},
};

inline StrategySetup load_strategy(const Config& cfg) {
  simdjson::dom::parser parser;
  const simdjson::dom::element doc = parser.load(cfg.strategy.params_file);
  StrategySetup s;
  s.allow_long = bool(doc["trade"]["allow_long"]);
  s.allow_short = bool(doc["trade"]["allow_short"]);
  s.history_days = int64_t(doc["history_days"]);
  for (const auto& entry : kStrategies) {
    if (entry.name == cfg.strategy.name) {
      s.strategy = entry.make(doc["indicator"]);
      return s;
    }
  }
  throw std::runtime_error("unknown strategy: " + cfg.strategy.name);
}

}  // namespace edge
