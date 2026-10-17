#pragma once
// Thread 1: Market Data socket for one instrument -> Tick queue (+ raw copies for the recorder).
// Uses the Market Data session logged in by main (shared with thread 3's history calls).
// On every (re)connect it subscribes 1501 Touchline plus the recorder's feeds (engine-config.json
// recorder.feeds, e.g. 1502 depth, 1505 candles, 1510 open interest). Ticks: a Tick is emitted only
// when TotalTradedQuantity moves (a trade happened); quote-only updates are dropped.
// Recorder: every subscribed event and every socket status change is copied, as received, to the
// raw queue for thread 6; when that queue is over recorder.max_queue the copy is dropped and counted
// (trading never waits for the recorder).
// 1501 payloads put the touchline fields in a "Touchline" object or at the top level; both are read
// (simdjson DOM here, since the lookup falls back from the child object to its parent).
#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "edge/core/config.hpp"
#include "edge/core/json.hpp"
#include "edge/core/log.hpp"
#include "edge/core/types.hpp"
#include "edge/net/rest_client.hpp"
#include "edge/net/socketio_client.hpp"
#include "edge/xts/session.hpp"

namespace edge {

class MarketDataThread {
 public:
  MarketDataThread(const Config& cfg, XtsSession md, Queue<Tick>& out, Queue<RawEvent>& raw)
      : cfg_(cfg), md_(std::move(md)), out_(out), raw_(raw), codes_(subscribed_codes(cfg)) {}

  void operator()(std::stop_token st) {
    set_thread_name("edge-md");
    try {
      RestClient http(cfg_.xts.md_base(), cfg_.engine.rest);
      http.set_token(md_.token);
      SocketIoClient socket(
          "md", cfg_.xts.host, md_socket_target(md_), cfg_.engine.socket,
          [this](std::string_view name, std::string_view data) { on_event(name, data); },
          [this, &http] { subscribe(http); },
          [this](std::string_view s) { record("socket", "\"" + json_escape(s) + "\""); });
      socket.run(st);
    } catch (const std::exception& e) {
      ELOG_ERROR("market data thread stopped: {}", e.what());
    }
  }

 private:
  static std::vector<std::int64_t> subscribed_codes(const Config& cfg) {
    std::vector<std::int64_t> codes{1501};
    if (cfg.engine.recorder.enabled)
      for (const auto c : cfg.engine.recorder.feeds)
        if (std::find(codes.begin(), codes.end(), c) == codes.end()) codes.push_back(c);
    return codes;
  }

  void subscribe(RestClient& http) {
    for (const auto code : codes_) {
      const auto body = std::format(
          R"({{"instruments":[{{"exchangeSegment":{},"exchangeInstrumentID":{}}}],"xtsMessageCode":{}}})",
          cfg_.instrument.segment_code, cfg_.instrument.id, code);
      const auto r = http.call("POST", "/instruments/subscription", body);
      if (r.ok()) ELOG_INFO("subscribed {} for {} ({})", code, cfg_.instrument.symbol, cfg_.instrument.id);
      else ELOG_WARN("{} subscription: HTTP {} {}", code, r.status, r.body.substr(0, 300));
    }
  }

  // Event names look like "1502-json-full": record those whose code is subscribed.
  bool recorded(std::string_view name) const {
    if (!cfg_.engine.recorder.enabled || name.size() < 4) return false;
    std::int64_t code = 0;
    if (std::from_chars(name.data(), name.data() + 4, code).ec != std::errc{}) return false;
    return std::find(codes_.begin(), codes_.end(), code) != codes_.end();
  }

  void record(std::string_view name, std::string data) {
    if (!cfg_.engine.recorder.enabled) return;
    if (static_cast<std::int64_t>(raw_.size_approx()) >= cfg_.engine.recorder.max_queue) {
      if (++dropped_ % 10'000 == 1) ELOG_WARN("recorder queue full: {} records dropped so far", dropped_);
      return;
    }
    raw_.enqueue(RawEvent{now_ns(), std::string(name), std::move(data)});
  }

  void on_event(std::string_view name, std::string_view data) {
    if (recorded(name)) record(name, std::string(data));
    if (name != "1501-json-full") return;
    const simdjson::dom::element top = dom_.parse(data.data(), data.size());
    auto tl_field = top["Touchline"];
    const simdjson::dom::element tl = tl_field.error() ? top : tl_field.value();

    const auto id = dom_number(top, "ExchangeInstrumentID");
    if (!id || static_cast<std::int64_t>(*id) != cfg_.instrument.id) return;
    const auto ltp = dom_number(tl, "LastTradedPrice");
    const auto ttq = dom_number(tl, "TotalTradedQuantity");
    auto ltt = dom_number(tl, "LastTradedTime");
    if (!ltt) ltt = dom_number(top, "LastTradedTime");
    if (!ltp || !ttq || !ltt) {
      ELOG_DEBUG("1501 without LTP/volume/time skipped: {}", data.substr(0, 200));
      return;
    }

    const auto volume = static_cast<std::int64_t>(*ttq);
    if (volume <= last_ttq_) return;
    const auto qty = last_ttq_ < 0 ? 0 : volume - last_ttq_;
    last_ttq_ = volume;
    out_.enqueue(Tick{cfg_.instrument.id, xts_to_unix_ms(static_cast<std::int64_t>(*ltt)), *ltp, qty});
  }

  const Config& cfg_;
  const XtsSession md_;
  Queue<Tick>& out_;
  Queue<RawEvent>& raw_;
  std::vector<std::int64_t> codes_;
  simdjson::dom::parser dom_;  // IXWebSocket thread only
  std::int64_t last_ttq_ = -1;
  std::uint64_t dropped_ = 0;
};

}  // namespace edge
