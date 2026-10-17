#pragma once
// OBV MACD v1.0 — port of TradingView "OBV MACD Indicator" (Pine v4) with MA type DEMA.
//  1. OBV        v = cum(sign(change(close)) * volume)
//  2. shadow   = (v - sma(v, v_len)) / stdev(v - sma, window_len) * stdev(high - low, window_len)
//     out      = shadow > 0 ? high + shadow : low + shadow;   obvema = ema(out, obv_len)
//  3. macd     = dema(obvema, ma_len) - ema(close, slow_len)
//  4. tt1      = end value of the linear regression of macd over reg_len bars (reg_len 2: tt1 == macd)
//  5. T-Channel (Alex Grover): a = cum(|tt1 - b5[1]|) / bar_index * channel_mult;
//     b5 steps to tt1 when tt1 leaves b5[1] +/- a, else holds; oc = direction of b5's last step
//  6. oc turns up (blue cross) -> Cross::Up; turns down (red cross) -> Cross::Down
// Pine semantics kept: ema() is seeded with the SMA of its first `len` values; stdev() is the
// population stdev; cum() skips na; division by zero gives na. The pivot/divergence labels are
// display-only and not ported; the other MA types (TEMA, HMA, ZLEMA...) are unused at DEMA.
// The T-Channel threshold averages over EVERY bar since the first one fed, so values depend on how
// much history is loaded: the strategy thread warms up on history_days of XTS bars.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge/strategy/strategy.hpp"

namespace edge {

struct ObvMacdParams {
  std::int64_t window_len, v_len, obv_len, ma_len, slow_len, reg_len;
  double channel_mult;
};

namespace pine {

inline constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
inline bool na(double x) { return std::isnan(x); }
inline double div(double a, double b) { return b == 0.0 ? kNa : a / b; }

// The last n values of a series; complete() once n values are in and none of them is na.
class Window {
 public:
  explicit Window(std::int64_t n) : buf_(static_cast<std::size_t>(n), kNa) {
    if (n < 1) throw std::invalid_argument("window length must be >= 1");
  }

  void push(double x) {
    if (count_ == buf_.size()) {
      if (na(buf_[pos_])) --nans_;
    } else {
      ++count_;
    }
    buf_[pos_] = x;
    if (na(x)) ++nans_;
    pos_ = (pos_ + 1) % buf_.size();
  }

  bool complete() const { return count_ == buf_.size() && nans_ == 0; }
  double at(std::size_t bars_ago) const { return buf_[(pos_ + buf_.size() - 1 - bars_ago) % buf_.size()]; }

  double mean() const {
    double s = 0;
    for (double x : buf_) s += x;
    return s / static_cast<double>(buf_.size());
  }

  double stdev() const {  // population, like Pine's stdev()
    const double m = mean();
    double s = 0;
    for (double x : buf_) s += (x - m) * (x - m);
    return std::sqrt(s / static_cast<double>(buf_.size()));
  }

 private:
  std::vector<double> buf_;
  std::size_t pos_ = 0, count_ = 0, nans_ = 0;
};

// Pine ema(): seeded with the SMA of the first `len` values, then alpha * x + (1 - alpha) * prev.
class Ema {
 public:
  explicit Ema(std::int64_t len) : alpha_(2.0 / static_cast<double>(len + 1)), seed_(len) {}

  double update(double x) {
    seed_.push(x);
    if (!na(value_) && !na(x)) return value_ = alpha_ * x + (1 - alpha_) * value_;
    return value_ = seed_.complete() ? seed_.mean() : kNa;
  }

 private:
  double alpha_;
  Window seed_;
  double value_ = kNa;
};

}  // namespace pine

class ObvMacd final : public Strategy {
 public:
  explicit ObvMacd(const ObvMacdParams& p)
      : p_(p), v_win_(p.v_len), dev_win_(p.window_len), range_win_(p.window_len), obv_ema_(p.obv_len),
        ma1_(p.ma_len), ma2_(p.ma_len), slow_(p.slow_len), macd_win_(p.reg_len) {
    if (p.reg_len < 2) throw std::invalid_argument("obv_macd: reg_len must be >= 2");
  }

  Cross on_candle(const Candle& c) override {
    using namespace pine;
    // 1-2. OBV in price units
    if (!na(prev_close_)) {
      const double ch = c.close - prev_close_;
      v_ += static_cast<double>((ch > 0) - (ch < 0)) * static_cast<double>(c.volume);
    }
    prev_close_ = c.close;
    v_win_.push(v_);
    const double smooth = v_win_.complete() ? v_win_.mean() : kNa;
    dev_win_.push(v_ - smooth);
    range_win_.push(c.high - c.low);
    const double v_spread = dev_win_.complete() ? dev_win_.stdev() : kNa;
    const double price_spread = range_win_.complete() ? range_win_.stdev() : kNa;
    const double shadow = div(v_ - smooth, v_spread) * price_spread;
    const double out = na(shadow) ? kNa : shadow > 0 ? c.high + shadow : c.low + shadow;

    // 3. MACD: DEMA of the OBV line minus EMA of close
    const double obv = obv_ema_.update(out);
    const double e1 = ma1_.update(obv);
    const double e2 = ma2_.update(e1);
    macd_ = 2 * e1 - e2 - slow_.update(c.close);

    // 4. linear-regression end value
    macd_win_.push(macd_);
    tt1_ = linreg_end();

    // 5. T-Channel
    const double base = na(b5_) ? tt1_ : b5_;  // nz(b5[1], tt1)
    const double diff = std::fabs(tt1_ - base);
    if (!na(diff)) cum_abs_ += diff;
    const double a = div(cum_abs_, static_cast<double>(bar_)) * p_.channel_mult;  // bar_ = n5 (0-based)
    const double b5 = (tt1_ > base + a || tt1_ < base - a) ? tt1_ : base;    // na comparisons are false
    const double step = b5 - b5_;
    const int oc = step > 0 ? 1 : step < 0 ? -1 : oc_;

    // 6. crosses
    const Cross x = oc > oc_ ? Cross::Up : oc < oc_ ? Cross::Down : Cross::None;
    b5_ = b5;
    oc_ = oc;
    ++bar_;
    return x;
  }

  bool ready() const override { return !pine::na(b5_) && oc_ != 0; }

  std::string state() const override {
    return std::format("macd={:.2f} tt1={:.2f} b5={:.2f} oc={}", macd_, tt1_, b5_, oc_);
  }

 private:
  // Pine calcSlope(src, len): least-squares line over the last len values, x = 2..len+1; value at its end.
  double linreg_end() const {
    if (!macd_win_.complete()) return pine::kNa;
    const auto n = static_cast<double>(p_.reg_len);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (std::int64_t i = 1; i <= p_.reg_len; ++i) {
      const double val = macd_win_.at(static_cast<std::size_t>(p_.reg_len - i));
      const double per = static_cast<double>(i) + 1.0;
      sx += per;
      sy += val;
      sxx += per * per;
      sxy += val * per;
    }
    const double slope = pine::div(n * sxy - sx * sy, n * sxx - sx * sx);
    const double intercept = sy / n - slope * sx / n + slope;
    return intercept + slope * n;
  }

  ObvMacdParams p_;
  pine::Window v_win_, dev_win_, range_win_;
  pine::Ema obv_ema_, ma1_, ma2_, slow_;
  pine::Window macd_win_;
  double prev_close_ = pine::kNa, v_ = 0;
  double macd_ = pine::kNa, tt1_ = pine::kNa, b5_ = pine::kNa, cum_abs_ = 0;
  int oc_ = 0;
  std::int64_t bar_ = 0;
};

}  // namespace edge
