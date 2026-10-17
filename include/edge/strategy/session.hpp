#pragma once
// Exchange session calendar + trading-time rules, from the session JSON (config/session-mcx.json):
//   regular {open, close}       local times (timezone_offset_minutes from UTC) for a normal day
//   overrides {date: {..}|null} special sessions; null = closed (holiday)
//   weekends_closed             Saturday/Sunday closed unless overridden
//   rules                       entry_start_after_open_min, entry_cutoff_before_close_min,
//                               square_off_before_close_min — relative to that day's close, so the
//                               summer (23:30) and winter (23:55) MCX closes need no separate rules
// phase(t) classifies a decision time (a bar's close) as Entry / NoEntry / SquareOff / Closed.
#include <simdjson.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace edge {

enum class Phase : std::uint8_t { Closed, Entry, NoEntry, SquareOff };

class Session {
 public:
  static Session load(const std::string& path) {
    simdjson::dom::parser parser;
    const simdjson::dom::element doc = parser.load(path);
    Session s;
    s.offset_min_ = int64_t(doc["timezone_offset_minutes"]);
    s.weekends_closed_ = bool(doc["weekends_closed"]);
    s.regular_ = hours(doc["regular"]);
    const auto r = doc["rules"];
    s.start_ = int64_t(r["entry_start_after_open_min"]);
    s.cutoff_ = int64_t(r["entry_cutoff_before_close_min"]);
    s.square_off_ = int64_t(r["square_off_before_close_min"]);
    for (const auto field : simdjson::dom::object(doc["overrides"])) {
      const auto day = day_number(field.key);
      s.overrides_[day] = field.value.is_null() ? std::nullopt : std::optional<Hours>(hours(field.value));
    }
    return s;
  }

  Phase phase(std::int64_t t_ms) const {
    const auto local = local_minutes(t_ms);
    const auto h = hours_for(local / 1440);
    if (!h) return Phase::Closed;
    const auto m = local % 1440;
    if (m < h->open || m > h->close) return Phase::Closed;
    if (m >= h->close - square_off_) return Phase::SquareOff;
    if (m >= h->close - cutoff_ || m < h->open + start_) return Phase::NoEntry;
    return Phase::Entry;
  }

  std::int64_t day_of(std::int64_t t_ms) const { return local_minutes(t_ms) / 1440; }  // local day number

 private:
  struct Hours {
    std::int64_t open, close;  // minutes of the local day
  };

  std::int64_t local_minutes(std::int64_t t_ms) const { return t_ms / 60'000 + offset_min_; }

  std::optional<Hours> hours_for(std::int64_t day) const {
    if (const auto it = overrides_.find(day); it != overrides_.end()) return it->second;
    const std::chrono::weekday wd{std::chrono::sys_days{std::chrono::days{day}}};
    if (weekends_closed_ && (wd == std::chrono::Saturday || wd == std::chrono::Sunday)) return std::nullopt;
    return regular_;
  }

  static std::int64_t num(std::string_view s) {
    std::int64_t v = 0;
    if (std::from_chars(s.data(), s.data() + s.size(), v).ec != std::errc{}) throw std::runtime_error("session: bad number");
    return v;
  }

  static std::int64_t minutes(std::string_view hhmmss) { return num(hhmmss.substr(0, 2)) * 60 + num(hhmmss.substr(3, 2)); }

  static Hours hours(simdjson::dom::element o) {
    return {minutes(std::string_view(o["open"])), minutes(std::string_view(o["close"]))};
  }

  static std::int64_t day_number(std::string_view ymd) {  // "2026-11-02" -> days since 1970-01-01
    using namespace std::chrono;
    const year_month_day d{year{static_cast<int>(num(ymd.substr(0, 4)))},
                           month{static_cast<unsigned>(num(ymd.substr(5, 2)))},
                           day{static_cast<unsigned>(num(ymd.substr(8, 2)))}};
    if (!d.ok()) throw std::runtime_error("session: bad date " + std::string(ymd));
    return sys_days{d}.time_since_epoch().count();
  }

  std::int64_t offset_min_ = 0, start_ = 0, cutoff_ = 0, square_off_ = 0;
  bool weekends_closed_ = true;
  Hours regular_{};
  std::map<std::int64_t, std::optional<Hours>> overrides_;
};

}  // namespace edge
