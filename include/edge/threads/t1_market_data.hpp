#pragma once
// Thread 1: Market Data socket for one instrument -> Tick queue.
// Subscribes to 1501 Touchline on every (re)connect and emits a Tick only when TotalTradedQuantity
// moves (a trade happened); quote-only updates are dropped.
#include <format>
#include <stop_token>

#include "edge/core/config.hpp"
#include "edge/core/json.hpp"
#include "edge/core/log.hpp"
#include "edge/net/socketio_client.hpp"
#include "edge/core/types.hpp"
#include "edge/xts/session.hpp"

namespace edge {

class MarketDataThread {
 public:
  MarketDataThread(const Config& cfg, Queue<Tick>& out) : cfg_(cfg), out_(out) {}

  void operator()(std::stop_token st) {
    set_thread_name("edge-md");
    try {
      RedisStore redis(cfg_.redis);
      RestClient http(cfg_.xts.md_base());
      const auto session = md_login(http, redis, cfg_.redis);
      http.set_token(session.token);
      ELOG_INFO("market data session up for {}", session.user_id);
      SocketIoClient socket(
          "md", cfg_.xts.host, md_socket_target(session),
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
    auto& d = json_.parse(data);
    const auto id = as_int(d["ExchangeInstrumentID"]);
    if (id != cfg_.instrument.id) return;
    auto tl = d["Touchline"];
    const double ltp = as_double(tl["LastTradedPrice"]);
    const auto ttq = as_int(tl["TotalTradedQuantity"]);
    const auto ltt = as_int(tl["LastTradedTime"]);
    if (ttq <= last_ttq_) return;
    const auto qty = last_ttq_ < 0 ? 0 : ttq - last_ttq_;
    last_ttq_ = ttq;
    out_.enqueue(Tick{id, xts_to_unix_ms(ltt), ltp, qty});
  }

  const Config& cfg_;
  Queue<Tick>& out_;
  JsonReader json_;
  std::int64_t last_ttq_ = -1;
};

}  // namespace edge
