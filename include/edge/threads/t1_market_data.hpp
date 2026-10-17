#pragma once
// Thread 1: Market Data socket for one instrument -> Tick queue.
// Uses the Market Data session logged in by main (shared with thread 3's history calls).
// Subscribes to 1501 Touchline on every (re)connect and emits a Tick only when TotalTradedQuantity
// moves (a trade happened); quote-only updates are dropped.
// 1501 payloads put the touchline fields in a "Touchline" object or at the top level; both are read
// (simdjson DOM here, since the lookup falls back from the child object to its parent).
#include <format>
#include <optional>
#include <stop_token>

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
  MarketDataThread(const Config& cfg, XtsSession md, Queue<Tick>& out) : cfg_(cfg), md_(std::move(md)), out_(out) {}

  void operator()(std::stop_token st) {
    set_thread_name("edge-md");
    try {
      RestClient http(cfg_.xts.md_base(), cfg_.engine.rest);
      http.set_token(md_.token);
      SocketIoClient socket(
          "md", cfg_.xts.host, md_socket_target(md_), cfg_.engine.socket,
          [this](std::string_view name, std::string_view data) { on_event(name, data); },
          [this, &http] { subscribe(http); });
      socket.run(st);
    } catch (const std::exception& e) {
      ELOG_ERROR("market data thread stopped: {}", e.what());
    }
  }

 private:
  void subscribe(RestClient& http) {
    const auto body = std::format(
        R"({{"instruments":[{{"exchangeSegment":{},"exchangeInstrumentID":{}}}],"xtsMessageCode":1501}})",
        cfg_.instrument.segment_code, cfg_.instrument.id);
    const auto r = http.call("POST", "/instruments/subscription", body);
    if (r.ok()) ELOG_INFO("subscribed 1501 for {} ({})", cfg_.instrument.symbol, cfg_.instrument.id);
    else ELOG_WARN("1501 subscription: HTTP {} {}", r.status, r.body.substr(0, 300));
  }

  void on_event(std::string_view name, std::string_view data) {
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
  simdjson::dom::parser dom_;  // IXWebSocket thread only
  std::int64_t last_ttq_ = -1;
};

}  // namespace edge
