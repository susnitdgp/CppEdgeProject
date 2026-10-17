#pragma once
// Thread 4: order engine. Consumes Signals (thread 3) and OrderUpdates (thread 5) from one queue.
// Target-position loop: target = signal x lots; while position != target, send ONE marketable limit
// order for the difference and wait for it to finish (the broker has no reduce-only, so two working
// orders could both fill). Position comes from our own fills and is kept in Redis across restarts.
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <stop_token>
#include <variant>

#include "edge/config.hpp"
#include "edge/json.hpp"
#include "edge/log.hpp"
#include "edge/redis_store.hpp"
#include "edge/rest_client.hpp"
#include "edge/types.hpp"
#include "edge/xts.hpp"

namespace edge {

class OrderEngineThread {
 public:
  OrderEngineThread(const Config& cfg, XtsSession ia, Queue<EngineEvent>& in)
      : cfg_(cfg), ia_(std::move(ia)), in_(in), pos_key_(std::format("position:{}", cfg.instrument.id)) {}

  void operator()(std::stop_token st) {
    set_thread_name("edge-orders");
    try {
      RedisStore redis(cfg_.redis);
      RestClient http(cfg_.xts.ia_base());
      http.set_token(ia_.token);
      redis_ = &redis;
      http_ = &http;
      const auto saved = redis.get_field(pos_key_, "lots");
      position_ = saved.empty() ? 0 : std::stoll(saved);
      target_ = position_;
      ELOG_INFO("order engine up: position {} lots, live orders {}", position_, cfg_.xts.live_orders_enabled);
      EngineEvent ev;
      while (!st.stop_requested()) {
        if (in_.wait_dequeue_timed(ev, kPollTimeout)) std::visit([this](const auto& e) { handle(e); }, ev);
      }
    } catch (const std::exception& e) {
      ELOG_ERROR("order engine stopped: {}", e.what());
    }
  }

 private:
  struct Working {
    OrderId id;
    std::int64_t sign, qty, filled;
  };

  void handle(const Signal& s) {
    target_ = static_cast<std::int64_t>(s.target) * cfg_.instrument.lots;
    ref_price_ = s.ref_price;
    ELOG_INFO("target {} lots (position {})", target_, position_);
    rebalance();
  }

  void handle(const OrderUpdate& u) {
    if (!working_ || view(u.unique_id) != view(working_->id)) return;
    if (const auto delta = u.cum_qty - working_->filled; delta > 0) {
      working_->filled = u.cum_qty;
      position_ += working_->sign * delta;
      redis_->set_fields(pos_key_, {{"lots", std::to_string(position_)},
                                    {"avg_price", std::to_string(u.avg_price)},
                                    {"updated_ms", std::to_string(now_ms())}});
      ELOG_INFO("fill {} x{} @ {} -> position {}", view(u.unique_id), delta, u.avg_price, position_);
    }
    if (is_terminal(u.status)) {
      ELOG_INFO("order {} {}", view(u.unique_id), to_string(u.status));
      working_.reset();
      if (u.status != OrderStatus::Filled) give_up();  // never auto-resend a rejected/cancelled order
      rebalance();
    }
  }

  void give_up() {
    ELOG_WARN("target {} abandoned; holding {} lots until the next signal", target_, position_);
    target_ = position_;
  }

  void rebalance() {
    if (working_) return;  // re-checked when the working order finishes
    if (const auto delta = target_ - position_; delta != 0) place(delta);
  }

  void place(std::int64_t delta) {
    const bool buy = delta > 0;
    const auto qty = std::llabs(delta);
    const auto& in = cfg_.instrument;
    const double raw = ref_price_ + (buy ? in.protect_points : -in.protect_points);
    const double limit = std::round(raw / in.tick_size) * in.tick_size;
    if (!cfg_.xts.live_orders_enabled) {
      ELOG_WARN("live orders disabled: would {} {} @ {}", buy ? "BUY" : "SELL", qty, limit);
      return;
    }
    const auto uid = std::format("{}{}{:02}", kOrderPrefix, now_ms(), ++seq_ % 100);  // 17 chars
    const auto body = std::format(
        R"({{"exchangeSegment":"{}","exchangeInstrumentID":{},"productType":"{}","orderType":"LIMIT",)"
        R"("orderSide":"{}","timeInForce":"DAY","disclosedQuantity":0,"orderQuantity":{},"limitPrice":{},)"
        R"("stopPrice":0,"orderUniqueIdentifier":"{}","clientID":"{}"}})",
        in.segment, in.id, in.product, buy ? "BUY" : "SELL", qty, limit, uid, json_escape(cfg_.xts.client_id));
    working_ = Working{make_order_id(uid), buy ? 1 : -1, qty, 0};
    redis_->set_fields("order:" + uid, {{"side", buy ? "BUY" : "SELL"},
                                        {"qty", std::to_string(qty)},
                                        {"limit", std::to_string(limit)},
                                        {"status", "PendingNew"},
                                        {"sent_ms", std::to_string(now_ms())}});
    try {
      const auto r = http_->call("POST", "/orders", body);
      if (!r.ok()) throw xts_error("place order", r);
      JsonReader json;
      const auto app_id = as_int(json.parse(r.body)["result"]["AppOrderID"]);
      redis_->set_fields("order:" + uid, {{"app_order_id", std::to_string(app_id)}});
      ELOG_INFO("placed {} {} {} @ {} -> AppOrderID {}", uid, buy ? "BUY" : "SELL", qty, limit, app_id);
    } catch (const std::exception& e) {
      ELOG_ERROR("order {} not placed: {}", uid, e.what());
      redis_->set_fields("order:" + uid, {{"status", "Rejected"}, {"reason", e.what()}});
      working_.reset();
      give_up();
    }
  }

  const Config& cfg_;
  const XtsSession ia_;
  Queue<EngineEvent>& in_;
  const std::string pos_key_;
  RedisStore* redis_ = nullptr;
  RestClient* http_ = nullptr;
  std::int64_t position_ = 0, target_ = 0, seq_ = 0;
  double ref_price_ = 0;
  std::optional<Working> working_;
};

}  // namespace edge
