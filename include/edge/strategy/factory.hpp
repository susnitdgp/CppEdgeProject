#pragma once
// config.json strategy {name, params_file} -> Strategy + its trade switches + warm-up length.
// The params file holds: "indicator" (strategy settings), "trade" {allow_long, allow_short},
// "history_days" (XTS history fed at start-up). Register each new strategy in load_strategy().
#include <simdjson.h>

#include <cstdint>
#include <memory>
#include <stdexcept>

#include "edge/core/config.hpp"
#include "edge/strategy/obv_macd.hpp"
#include "edge/strategy/strategy.hpp"

namespace edge {

struct StrategySetup {
  std::unique_ptr<Strategy> strategy;
  bool allow_long = false, allow_short = false;
  std::int64_t history_days = 0;
};

inline StrategySetup load_strategy(const Config& cfg) {
  simdjson::dom::parser parser;
  const simdjson::dom::element doc = parser.load(cfg.strategy.params_file);
  StrategySetup s;
  s.allow_long = bool(doc["trade"]["allow_long"]);
  s.allow_short = bool(doc["trade"]["allow_short"]);
  s.history_days = int64_t(doc["history_days"]);
  const auto in = doc["indicator"];
  if (cfg.strategy.name == "obv_macd") {
    s.strategy = std::make_unique<ObvMacd>(ObvMacdParams{
        int64_t(in["window_len"]), int64_t(in["v_len"]), int64_t(in["obv_len"]), int64_t(in["ma_len"]),
        int64_t(in["slow_len"]), int64_t(in["reg_len"]), double(in["channel_mult"])});
  } else {
    throw std::runtime_error("unknown strategy: " + cfg.strategy.name);
  }
  return s;
}

}  // namespace edge
