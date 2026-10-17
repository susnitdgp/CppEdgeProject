#pragma once
// XTS historical OHLC: GET /apimarketdata/instruments/ohlc -> finished Candles, oldest first.
// CompositeEdge (probed 13 Oct 2026 by the Rust adapter): startTime/endTime are IST strings like
// "Oct 13 2026 090000"; compressionValue is in seconds (60, 180, 300 work); result.dataReponse (sic)
// is "time|open|high|low|close|volume|oi|,..." where time is the bar's LAST second in IST-epoch
// seconds. The newest bar may still be forming and is dropped. One call returned ~8 000 bars, so
// a month of 5-minute bars is a single request.
#include <simdjson.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "edge/core/config.hpp"
#include "edge/core/types.hpp"
#include "edge/net/rest_client.hpp"
#include "edge/xts/session.hpp"

namespace edge {

// Unix UTC ms -> XTS time string in IST, e.g. "Oct 13 2026 090000".
inline std::string xts_time(std::int64_t unix_ms) {
  using namespace std::chrono;
  return std::format("{:%b %d %Y %H%M%S}", sys_seconds{seconds{unix_ms / 1000 + 19'800}});
}

inline std::vector<Candle> fetch_history(RestClient& md, const Config& cfg, std::int64_t from_ms, std::int64_t to_ms) {
  const std::int64_t step_s = cfg.candle.timeframe_minutes * 60;
  auto enc = [](std::string s) {
    std::string out;
    for (char ch : s) out += ch == ' ' ? std::string("%20") : std::string(1, ch);
    return out;
  };
  const auto path = std::format(
      "/instruments/ohlc?exchangeSegment={}&exchangeInstrumentID={}&startTime={}&endTime={}&compressionValue={}",
      cfg.instrument.segment, cfg.instrument.id, enc(xts_time(from_ms)), enc(xts_time(to_ms)), step_s);
  const auto r = md.call("GET", path);
  if (!r.ok()) throw xts_error("history", r);

  simdjson::dom::parser parser;
  const simdjson::dom::element doc = parser.parse(r.body);
  std::string_view data;
  if (doc["result"]["dataReponse"].get_string().get(data) != simdjson::SUCCESS &&
      doc["result"]["dataResponse"].get_string().get(data) != simdjson::SUCCESS)
    return {};

  auto num = [](std::string_view s) {
    double d = 0;
    std::from_chars(s.data(), s.data() + s.size(), d);
    return d;
  };
  const auto now = now_ms();
  std::vector<Candle> bars;
  while (!data.empty()) {
    const auto comma = data.find(',');
    const auto row = data.substr(0, comma);
    data = comma == std::string_view::npos ? std::string_view{} : data.substr(comma + 1);
    std::string_view f[7];
    std::size_t n = 0;
    for (std::string_view rest = row; n < 7 && !rest.empty(); ++n) {
      const auto bar = rest.find('|');
      f[n] = rest.substr(0, bar);
      rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
    }
    if (n < 6) continue;
    const auto end_s = static_cast<std::int64_t>(num(f[0]));
    const Candle c{cfg.instrument.id, xts_to_unix_ms(end_s + 1 - step_s), num(f[1]), num(f[2]),
                   num(f[3]), num(f[4]), static_cast<std::int64_t>(num(f[5]))};
    if (c.open_ts_ms + step_s * 1000 <= now && c.open > 0 && c.high >= c.low) bars.push_back(c);
  }
  std::sort(bars.begin(), bars.end(), [](const Candle& a, const Candle& b) { return a.open_ts_ms < b.open_ts_ms; });
  bars.erase(std::unique(bars.begin(), bars.end(),
                         [](const Candle& a, const Candle& b) { return a.open_ts_ms == b.open_ts_ms; }),
             bars.end());
  return bars;
}

}  // namespace edge
