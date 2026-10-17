#pragma once
// Pine Script building blocks with TradingView semantics, shared by the ported strategies.
//  - na is NaN; comparisons with na are false; division by zero gives na
//  - Window: the last n values of a series; na-aware (complete() only when n values and no na)
//  - Ema: Pine ema()/rma(): seeded with the SMA of the first len values, then
//         alpha * x + (1 - alpha) * prev  (ema alpha = 2/(len+1), rma alpha = 1/len)
//  - Window::stdev() is the population stdev, like Pine's stdev(); wma() like ta.wma(); sum() like math.sum()
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace edge::pine {

inline constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
inline bool na(double x) { return std::isnan(x); }
inline double div(double a, double b) { return b == 0.0 ? kNa : a / b; }

class Window {
 public:
  explicit Window(std::int64_t n) : buf_(static_cast<std::size_t>(n < 1 ? 1 : n), kNa) {
    if (n < 1) throw std::invalid_argument("pine window length must be >= 1");
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
  std::size_t size() const { return buf_.size(); }
  double at(std::size_t bars_ago) const { return buf_[(pos_ + buf_.size() - 1 - bars_ago) % buf_.size()]; }

  double sum() const {
    if (!complete()) return kNa;
    double s = 0;
    for (double x : buf_) s += x;
    return s;
  }

  double mean() const { return sum() / static_cast<double>(buf_.size()); }

  double stdev() const {  // population
    const double m = mean();
    if (na(m)) return kNa;
    double s = 0;
    for (double x : buf_) s += (x - m) * (x - m);
    return std::sqrt(s / static_cast<double>(buf_.size()));
  }

  double wma() const {  // newest value weighted n, oldest weighted 1
    if (!complete()) return kNa;
    const auto n = buf_.size();
    double s = 0;
    for (std::size_t i = 0; i < n; ++i) s += at(i) * static_cast<double>(n - i);
    return s / (static_cast<double>(n) * static_cast<double>(n + 1) / 2.0);
  }

 private:
  std::vector<double> buf_;
  std::size_t pos_ = 0, count_ = 0, nans_ = 0;
};

class Ema {
 public:
  static Ema ema(std::int64_t len) { return Ema(len, 2.0 / static_cast<double>(len + 1)); }
  static Ema rma(std::int64_t len) { return Ema(len, 1.0 / static_cast<double>(len)); }

  double update(double x) {
    seed_.push(x);
    if (!na(value_) && !na(x)) return value_ = alpha_ * x + (1 - alpha_) * value_;
    return value_ = seed_.complete() ? seed_.mean() : kNa;
  }

 private:
  Ema(std::int64_t len, double alpha) : alpha_(alpha), seed_(len) {}
  double alpha_;
  Window seed_;
  double value_ = kNa;
};

}  // namespace edge::pine
