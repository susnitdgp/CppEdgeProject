#pragma once
// AlphaTrend v1.0 — port of TradingView "AlphaTrend" by KivancOzbilgic (Pine v5).
//  1. ATR      = hma(true range, period)       (Hull MA: wma(2*wma(x, n/2) - wma(x, n), floor(sqrt(n))))
//  2. momentum = mfi(hlc3, period) >= 50       (use_rsi: rsi(close, period) >= 50, for no volume data)
//  3. AT       = bullish ? max-ratchet(low - ATR*coeff) : min-ratchet(high + ATR*coeff)
//                i.e. bullish: upT if upT >= AT[1] else AT[1]; bearish: downT if downT <= AT[1] else AT[1]
//                (AT[1] is nz(AT[1]) = 0 before the first value, as in Pine)
//  4. raw BUY  = crossover(AT, AT[2]); raw SELL = crossunder(AT, AT[2])
//  5. shown BUY only if barssince(buy[1]) > barssince(sell) — the last signal was a SELL — and vice
//     versa, so BUY and SELL alternate; these are the chart labels -> Cross::Up / Cross::Down
// Settings: config/strategies/alpha_trend.json "indicator" (from_json). Pine semantics come from pine.hpp. Display-only parts (fill colours, price-cross alerts) are not ported.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <memory>
#include <string>

#include <simdjson.h>

#include "edge/strategy/pine.hpp"
#include "edge/strategy/strategy.hpp"

namespace edge {

struct AlphaTrendParams {
  double coeff;
  std::int64_t period;
  bool use_rsi;
};

class AlphaTrend final : public Strategy {
 public:
  static std::unique_ptr<Strategy> from_json(simdjson::dom::element in) {
    return std::make_unique<AlphaTrend>(
        AlphaTrendParams{double(in["coeff"]), int64_t(in["period"]), bool(in["use_rsi"])});
  }

  explicit AlphaTrend(const AlphaTrendParams& p)
      : p_(p), tr_half_(p.period / 2), tr_full_(p.period),
        hma_(static_cast<std::int64_t>(std::floor(std::sqrt(static_cast<double>(p.period))))),
        up_flow_(p.period), down_flow_(p.period), gain_(pine::Ema::rma(p.period)), loss_(pine::Ema::rma(p.period)) {
    if (p.period < 4) throw std::invalid_argument("alpha_trend: period must be >= 4");
  }

  Cross on_candle(const Candle& c) override {
    using namespace pine;
    // 1. ATR = HMA of true range (na on the first bar, as ta.tr)
    const double tr = na(prev_close_) ? kNa
                                      : std::max({c.high - c.low, std::fabs(c.high - prev_close_),
                                                  std::fabs(c.low - prev_close_)});
    tr_half_.push(tr);
    tr_full_.push(tr);
    hma_.push(2 * tr_half_.wma() - tr_full_.wma());
    atr_ = hma_.wma();

    // 2. momentum
    if (p_.use_rsi) {
      const double ch = c.close - prev_close_;  // na on the first bar
      const double up = gain_.update(na(ch) ? kNa : std::max(ch, 0.0));
      const double dn = loss_.update(na(ch) ? kNa : std::max(-ch, 0.0));
      momentum_ = na(up) || na(dn) ? kNa : dn == 0 ? 100.0 : up == 0 ? 0.0 : 100.0 - 100.0 / (1.0 + up / dn);
    } else {
      const double hlc3 = (c.high + c.low + c.close) / 3.0;
      const double ch = hlc3 - prev_hlc3_;  // na on the first bar; na comparisons are false, as in Pine
      const auto vol = static_cast<double>(c.volume);
      up_flow_.push(vol * (ch <= 0 ? 0.0 : hlc3));
      down_flow_.push(vol * (ch >= 0 ? 0.0 : hlc3));
      momentum_ = 100.0 - 100.0 / (1.0 + div(up_flow_.sum(), down_flow_.sum()));
      prev_hlc3_ = hlc3;
    }
    prev_close_ = c.close;
    const bool bullish = momentum_ >= 50;  // false while na

    // 3. AlphaTrend ratchet
    const double prev = na(at_[0]) ? 0.0 : at_[0];  // nz(AlphaTrend[1])
    const double up_t = c.low - atr_ * p_.coeff;
    const double down_t = c.high + atr_ * p_.coeff;
    const double at = bullish ? (up_t < prev ? prev : up_t) : (down_t > prev ? prev : down_t);
    at_[3] = at_[2];
    at_[2] = at_[1];
    at_[1] = at_[0];
    at_[0] = at;

    // 4-5. crossover / crossunder of AT with AT[2], alternating
    const bool raw_buy = at_[0] > at_[2] && at_[1] <= at_[3];
    const bool raw_sell = at_[0] < at_[2] && at_[1] >= at_[3];
    Cross x = Cross::None;
    if (raw_buy) {
      // barssince(buy[1]) > barssince(sell): (bar - last_buy - 1) > (bar - last_sell)
      if (last_buy_ >= 0 && last_sell_ >= 0 && bar_ - last_buy_ - 1 > bar_ - last_sell_) x = Cross::Up;
      last_buy_ = bar_;
    } else if (raw_sell) {
      if (last_sell_ >= 0 && last_buy_ >= 0 && bar_ - last_sell_ - 1 > bar_ - last_buy_) x = Cross::Down;
      last_sell_ = bar_;
    }
    ++bar_;
    return x;
  }

  bool ready() const override { return !pine::na(atr_) && !pine::na(momentum_) && last_buy_ >= 0 && last_sell_ >= 0; }

  std::string state() const override {
    return std::format("at={:.2f} at2={:.2f} atr={:.2f} {}={:.1f}", at_[0], at_[2], atr_, p_.use_rsi ? "rsi" : "mfi",
                       momentum_);
  }

 private:
  AlphaTrendParams p_;
  pine::Window tr_half_, tr_full_, hma_, up_flow_, down_flow_;
  pine::Ema gain_, loss_;
  double prev_close_ = pine::kNa, prev_hlc3_ = pine::kNa;
  double atr_ = pine::kNa, momentum_ = pine::kNa;
  double at_[4] = {pine::kNa, pine::kNa, pine::kNa, pine::kNa};  // AT, AT[1], AT[2], AT[3]
  std::int64_t bar_ = 0, last_buy_ = -1, last_sell_ = -1;
};

}  // namespace edge
