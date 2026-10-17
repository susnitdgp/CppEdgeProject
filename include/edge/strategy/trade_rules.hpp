#pragma once
// Cross + session -> target position (+1 long, 0 flat, -1 short). Shared by the live strategy
// thread and the backtest, so both trade exactly the same way.
//   Up cross:   exits a short; enters long only if allow_long and the session allows entries
//   Down cross: exits a long;  enters short only if allow_short and the session allows entries
//   (both allowed = the cross reverses; one side allowed = that side's entry, then exit on the next cross)
//   SquareOff / Closed phase: flat
//   New session day with a position still open: flat (MIS does not carry over)
#include <cstdint>
#include <optional>

#include "edge/strategy/session.hpp"
#include "edge/strategy/strategy.hpp"

namespace edge {

struct Decision {
  std::int32_t target;
  const char* reason;  // "cross", "square_off", "session_closed", "new_day"
};

class TradeRules {
 public:
  TradeRules(Session session, bool allow_long, bool allow_short)
      : session_(std::move(session)), allow_long_(allow_long), allow_short_(allow_short) {}

  // False for bars outside the session calendar (XTS sends stray off-session bars, e.g. Saturday
  // 15:25 with a few lots); such bars must not reach the indicator, live or in the backtest.
  bool in_session(std::int64_t bar_close_ms) const { return session_.phase(bar_close_ms) != Phase::Closed; }

  // decision_ms: the bar's close time (Unix UTC ms); current: the position the strategy holds.
  // Returns the new target, or nullopt when it does not change.
  std::optional<Decision> decide(Cross cross, std::int64_t decision_ms, std::int32_t current) {
    const auto day = session_.day_of(decision_ms);
    const bool new_day = day_ >= 0 && day != day_;
    day_ = day;
    const Phase phase = session_.phase(decision_ms);
    const bool can_enter = phase == Phase::Entry;

    Decision d{new_day ? 0 : current, new_day ? "new_day" : "cross"};
    if (phase == Phase::SquareOff || phase == Phase::Closed) {
      d = {0, phase == Phase::SquareOff ? "square_off" : "session_closed"};
    } else if (cross == Cross::Up) {
      d.target = can_enter && allow_long_ ? 1 : (d.target < 0 ? 0 : d.target);
    } else if (cross == Cross::Down) {
      d.target = can_enter && allow_short_ ? -1 : (d.target > 0 ? 0 : d.target);
    }
    if (d.target == current) return std::nullopt;
    return d;
  }

 private:
  Session session_;
  bool allow_long_, allow_short_;
  std::int64_t day_ = -1;
};

}  // namespace edge
