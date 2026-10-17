// backtest v1.1: replays bars through the live strategy code (Strategy + TradeRules + session rules),
// so a backtest trades exactly as the bot would. Settings: config/backtest.json
//   config        the bot's config.json (instrument, strategy, session file are taken from it)
//   source        "xts" = fetch history from XTS and save it to csv_path; "csv" = read csv_path
//   from, to      data range (IST dates, to inclusive); bars before trade_from only warm the indicator
//   costs         round_trip_points (taxes/fees per lot), slippage_points_per_side, brokerage_per_order
//   point_value   rupees per point per lot (CRUDEOILM: 10)
//   trades_csv, signals_csv   outputs (signals = every cross with indicator values, for chart checks)
// Fills: at the deciding bar's close +/- slippage (the bot sends a marketable limit at that close).
// Usage: backtest [config/backtest.json]   (source "xts" needs the Redis password, as for run.sh)
#include <curl/curl.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "edge/core/config.hpp"
#include "edge/core/types.hpp"
#include "edge/net/rest_client.hpp"
#include "edge/store/redis_store.hpp"
#include "edge/strategy/factory.hpp"
#include "edge/strategy/session.hpp"
#include "edge/strategy/trade_rules.hpp"
#include "edge/xts/history.hpp"
#include "edge/xts/session.hpp"

namespace {
using namespace edge;

struct Settings {
  std::string config, source, csv_path, trades_csv, signals_csv;
  std::int64_t from_ms, to_ms, trade_from_ms;
  double round_trip_points, slippage, brokerage, point_value;
};

std::int64_t ist_midnight_ms(std::string_view ymd) {  // "2026-09-15" 00:00 IST -> Unix UTC ms
  using namespace std::chrono;
  auto num = [](std::string_view s) {
    int v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
  };
  const year_month_day d{year{num(ymd.substr(0, 4))}, month{static_cast<unsigned>(num(ymd.substr(5, 2)))},
                         day{static_cast<unsigned>(num(ymd.substr(8, 2)))}};
  if (!d.ok()) throw std::runtime_error("bad date " + std::string(ymd));
  return duration_cast<milliseconds>(sys_days{d}.time_since_epoch()).count() - 19'800'000;
}

std::string ist(std::int64_t ms) {
  using namespace std::chrono;
  return std::format("{:%Y-%m-%d %H:%M}", sys_seconds{seconds{ms / 1000 + 19'800}});
}

Settings load_settings(const std::string& path) {
  simdjson::dom::parser parser;
  const simdjson::dom::element d = parser.load(path);
  auto s = [&d](const char* k) { return std::string(std::string_view(d[k])); };
  const auto c = d["costs"];
  return {s("config"), s("source"), s("csv_path"), s("trades_csv"), s("signals_csv"),
          ist_midnight_ms(s("from")), ist_midnight_ms(s("to")) + 86'400'000, ist_midnight_ms(s("trade_from")),
          double(c["round_trip_points"]), double(c["slippage_points_per_side"]), double(c["brokerage_per_order"]),
          double(d["point_value"])};
}

std::ofstream open_out(const std::string& path) {
  if (const auto dir = std::filesystem::path(path).parent_path(); !dir.empty())
    std::filesystem::create_directories(dir);
  std::ofstream f(path);
  if (!f) throw std::runtime_error("cannot write " + path);
  return f;
}

void write_csv(const std::string& path, const std::vector<Candle>& bars) {
  auto f = open_out(path);
  f << "time_ms,open,high,low,close,volume\n";
  for (const auto& b : bars) f << std::format("{},{},{},{},{},{}\n", b.open_ts_ms, b.open, b.high, b.low, b.close, b.volume);
}

std::vector<Candle> read_csv(const std::string& path, std::int64_t id) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot read " + path + " (run once with source \"xts\" to create it)");
  std::vector<Candle> bars;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] < '0' || line[0] > '9') continue;  // header
    double v[6]{};
    std::size_t start = 0;
    for (int i = 0; i < 6; ++i) {
      const auto comma = line.find(',', start);
      const auto field = line.substr(start, comma - start);
      std::from_chars(field.data(), field.data() + field.size(), v[i]);
      start = comma == std::string::npos ? line.size() : comma + 1;
    }
    const auto t = static_cast<std::int64_t>(v[0]);
    bars.push_back({id, t < 100'000'000'000 ? t * 1000 : t, v[1], v[2], v[3], v[4], static_cast<std::int64_t>(v[5])});
  }
  return bars;
}

struct Trade {
  std::int32_t side;
  std::int64_t entry_ms, exit_ms;
  double entry, exit;
  std::string reason;
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto bt = load_settings(argc > 1 ? argv[1] : "config/backtest.json");
    const Config cfg = load_config(bt.config);
    const std::int64_t tf_ms = cfg.candle.timeframe_minutes * 60'000;

    std::vector<Candle> bars;
    if (bt.source == "xts") {
      curl_global_init(CURL_GLOBAL_DEFAULT);
      RedisStore redis(cfg.redis);
      RestClient http(cfg.xts.md_base(), cfg.engine.rest);
      const auto md = md_login(http, redis, cfg.redis);
      http.set_token(md.token);
      bars = fetch_history(http, cfg, bt.from_ms, bt.to_ms);
      curl_global_cleanup();
      write_csv(bt.csv_path, bars);
      std::printf("fetched %zu bars from XTS, saved to %s\n", bars.size(), bt.csv_path.c_str());
    } else {
      bars = read_csv(bt.csv_path, cfg.instrument.id);
    }
    std::erase_if(bars, [&](const Candle& b) { return b.open_ts_ms < bt.from_ms || b.open_ts_ms >= bt.to_ms; });
    if (bars.empty()) throw std::runtime_error("no bars in range");

    auto setup = load_strategy(cfg);
    TradeRules rules(Session::load(cfg.session_file), setup.allow_long, setup.allow_short);
    auto signals = open_out(bt.signals_csv);
    signals << "bar_close_ist,cross,close,state\n";

    std::vector<Trade> trades;
    Trade open{};
    std::int32_t pos = 0;
    std::size_t warm = 0;
    std::size_t off_session = 0;
    for (const auto& b : bars) {
      const auto close_ms = b.open_ts_ms + tf_ms;
      if (!rules.in_session(close_ms)) {  // stray off-session bars never reach the indicator
        ++off_session;
        continue;
      }
      const Cross x = setup.strategy->on_candle(b);
      if (x != Cross::None)
        signals << std::format("{},{},{},{}\n", ist(close_ms), to_string(x), b.close, setup.strategy->state());
      if (b.open_ts_ms < bt.trade_from_ms || !setup.strategy->ready()) {
        ++warm;
        continue;
      }
      const auto d = rules.decide(x, close_ms, pos);
      if (!d) continue;
      if (pos != 0) {
        open.exit_ms = close_ms;
        open.exit = b.close - pos * bt.slippage;
        open.reason = d->reason;
        trades.push_back(open);
      }
      if (d->target != 0) open = Trade{d->target, close_ms, 0, b.close + d->target * bt.slippage, 0, ""};
      pos = d->target;
    }
    if (pos != 0) {
      open.exit_ms = bars.back().open_ts_ms + tf_ms;
      open.exit = bars.back().close - pos * bt.slippage;
      open.reason = "end";
      trades.push_back(open);
    }

    // ---- results
    const auto lots = static_cast<double>(cfg.instrument.lots);
    const double cost_rs = bt.round_trip_points * bt.point_value * lots + 2 * bt.brokerage;
    auto out = open_out(bt.trades_csv);
    out << "entry_ist,exit_ist,side,entry,exit,points,cost_rs,net_rs,reason\n";
    double gross_pts = 0, net_rs = 0, win_rs = 0, loss_rs = 0, peak = 0, max_dd = 0;
    std::size_t wins = 0, longs = 0;
    std::map<std::string, std::size_t> reasons;
    for (const auto& t : trades) {
      const double pts = t.side * (t.exit - t.entry);
      const double net = pts * bt.point_value * lots - cost_rs;
      gross_pts += pts;
      net_rs += net;
      (net > 0 ? win_rs : loss_rs) += net;
      wins += net > 0;
      longs += t.side > 0;
      ++reasons[t.reason];
      peak = std::max(peak, net_rs);
      max_dd = std::max(max_dd, peak - net_rs);
      out << std::format("{},{},{},{:.1f},{:.1f},{:.1f},{:.0f},{:.0f},{}\n", ist(t.entry_ms), ist(t.exit_ms),
                         t.side > 0 ? "LONG" : "SHORT", t.entry, t.exit, pts, cost_rs, net, t.reason);
    }

    const auto n = trades.size();
    std::printf("\nbacktest %s %s %lldm  (strategy %s, long %s, short %s)\n", cfg.instrument.symbol.c_str(),
                cfg.strategy.name.c_str(), static_cast<long long>(cfg.candle.timeframe_minutes),
                cfg.strategy.name.c_str(), setup.allow_long ? "on" : "off", setup.allow_short ? "on" : "off");
    std::printf("bars %zu (%zu warm-up, %zu off-session skipped), trading %s .. %s\n", bars.size(), warm,
                off_session, ist(bt.trade_from_ms).c_str(),
                ist(bars.back().open_ts_ms + tf_ms).c_str());
    std::printf("trades %zu (long %zu, short %zu), wins %zu (%.1f%%)\n", n, longs, n - longs, wins,
                n ? 100.0 * static_cast<double>(wins) / static_cast<double>(n) : 0.0);
    std::printf("gross %.1f pts, costs %.0f per trade (%.1f pts round trip + 2 x %.0f brokerage), slippage %.1f/side\n",
                gross_pts, cost_rs, bt.round_trip_points, bt.brokerage, bt.slippage);
    std::printf("net %.0f, avg %.0f per trade, profit factor %.2f, max drawdown %.0f (rupees, %g lot)\n", net_rs,
                n ? net_rs / static_cast<double>(n) : 0.0, loss_rs < 0 ? win_rs / -loss_rs : 0.0, max_dd, lots);
    std::printf("exits:");
    for (const auto& [reason, count] : reasons) std::printf(" %s %zu", reason.c_str(), count);
    std::printf("\ntrades: %s\nsignals: %s\n", bt.trades_csv.c_str(), bt.signals_csv.c_str());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "backtest: FAILED: %s\n", e.what());
    return 1;
  }
}
