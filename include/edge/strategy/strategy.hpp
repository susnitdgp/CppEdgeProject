#pragma once
// Strategy interface. on_candle() returns a new target position (+1 long, -1 short, 0 flat) only when
// it changes; std::nullopt means "no change". Called from thread 3 only.
#include <cstdint>
#include <optional>

#include "edge/core/types.hpp"

namespace edge {

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual std::optional<std::int32_t> on_candle(const Candle& c) = 0;
};

}  // namespace edge
