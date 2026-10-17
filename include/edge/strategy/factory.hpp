#pragma once
// config "strategy.name" -> Strategy instance. Register each new strategy here.
#include <memory>
#include <stdexcept>

#include "edge/core/config.hpp"
#include "edge/strategy/ema_cross.hpp"
#include "edge/strategy/strategy.hpp"

namespace edge {

inline std::unique_ptr<Strategy> make_strategy(const Config::Strategy& s) {
  if (s.name == "ema_cross") return std::make_unique<EmaCross>(s.fast, s.slow);
  throw std::runtime_error("unknown strategy: " + s.name);
}

}  // namespace edge
