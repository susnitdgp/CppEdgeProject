#pragma once
// Strategy interface. A strategy reads closed bars and reports its crosses. What a cross does to the
// position (long/short allowed, session entry window, square-off) is decided by TradeRules
// (trade_rules.hpp), which the live strategy thread and the backtest share.
#include <cstdint>
#include <string>

#include "edge/core/types.hpp"

namespace edge {

enum class Cross : std::int8_t { None = 0, Up = 1, Down = -1 };

inline const char* to_string(Cross c) { return c == Cross::Up ? "UP" : c == Cross::Down ? "DOWN" : "-"; }

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual Cross on_candle(const Candle& c) = 0;  // one call per closed bar, oldest first
  virtual bool ready() const = 0;                // indicators have values (enough history fed)
  virtual std::string state() const = 0;         // current indicator values, for logs and the backtest
};

}  // namespace edge
