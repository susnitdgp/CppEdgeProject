#pragma once
// Thread 4: order engine. Consumes Signals (thread 3) and OrderUpdates (thread 5) from one queue.
// Target-position loop: target = signal x lots; while position != target, send ONE marketable limit
// order for the difference and wait for it to finish (the broker has no reduce-only, so two working
// orders could both fill). Rejected/cancelled orders are never re-sent; the next signal decides.
//
// State: <p>position:<id> in Redis is the position of record. It is read before every order, and
// written on every fill and by the reconciliation poll.
// Reconciliation: whenever the order connection has been idle reconcile_interval_ms
// (engine-config.json), GET the broker's net
// positions (this also keeps the TCP+TLS connection warm). If the broker's quantity for this
// instrument+product differs from Redis while no order is working, the broker wins: Redis and the
// target are set to the broker quantity. Each check is recorded in <p>recon.
// Quantities: the engine works in lots; XTS order/fill/position quantities are lots x qty_per_lot.
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>

#include "edge/core/config.hpp"
#include "edge/core/json.hpp"
#include "edge/core/log.hpp"
#include "edge/core/types.hpp"
#include "edge/net/rest_client.hpp"
#include "edge/store/redis_store.hpp"
#include "edge/xts/session.hpp"

namespace edge {

class OrderEngineThread {
 public:
  OrderEngineThread(const Config& cfg, XtsSession ia, Queue<EngineEvent>& in)
      : cfg_(cfg), ia_(std::move(ia)), in_(in), pos_key_(std::format("position:{}", cfg.instrument.id)),
        unit_(cfg.instrument.qty_per_lot) {}

  void operator()(std::stop_token st) {
    set_thread_name("edge-orders");
    try {
      RedisStore redis(cfg_.redis);
      RestClient http(cfg_.xts.ia_base(), cfg_.engine.rest);
      http.set_token(ia_.token);
      redis_ = &redis;
      http_ = &http;
      load_position();
      target_ = position_;
      ELOG_INFO("order engine up: position {} lots, live orders {}", position_, cfg_.xts.live_orders_enabled);
      EngineEvent ev;
      while (!st.stop_requested()) {
        if (in_.wait_dequeue_timed(ev, cfg_.engine.queue_poll())) std::visit([this](const auto& e) { handle(e); }, ev);
        if (http.idle_ms() >= cfg_.engine.order_engine.reconcile_interval_ms) reconcile();  // first pass runs at once: connection warm from start
      }
    } catch (const std::exception& e) {
      ELOG_ERROR("order engine stopped: {}", e.what());
    }
  }

 private:

  struct Working {
    OrderId id;
    std::int64_t sign, qty, filled, sent_ms;  // qty/filled in lots
    bool stale_warned = false;
  };

  // ---------------------------------------------------------------- Redis position of record
  void load_position() {
    const auto v = redis_->get_field(pos_key_, "lots");
    position_ = v.empty() ? 0 : std::stoll(v);
  }

  void save_position(std::string_view source) {
    redis_->set_fields(pos_key_, {{"lots", std::to_string(position_)},
                                  {"source", std::string(source)},
                                  {"updated_ms", std::to_string(now_ms())}});
  }

  // ---------------------------------------------------------------- events
  void handle(const Signal& s) {
    target_ = static_cast<std::int64_t>(s.target) * cfg_.instrument.lots;
    ref_price_ = s.ref_price;
    ELOG_INFO("target {} lots (position {})", target_, position_);
    rebalance();
  }

  void handle(const OrderUpdate& u) {
    if (!working_ || view(u.unique_id) != view(working_->id)) return;
    const auto filled = u.cum_qty / unit_;
    if (const auto delta = filled - working_->filled; delta > 0) {
      working_->filled = filled;
      position_ += working_->sign * delta;
      save_position("fill");
      redis_->set_fields(pos_key_, {{"avg_price", std::to_string(u.avg_price)}});
      ELOG_INFO("fill {} x{} lots @ {} -> position {}", view(u.unique_id), delta, u.avg_price, position_);
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

  // ---------------------------------------------------------------- orders
  void rebalance() {
    if (working_) return;  // re-checked when the working order finishes
    load_position();       // Redis is the position of record (the poll may have corrected it)
    if (const auto delta = target_ - position_; delta != 0) place(delta);
  }

  void place(std::int64_t delta) {
    const bool buy = delta > 0;
    const auto lots = std::llabs(delta);
    const auto& in = cfg_.instrument;
    const double raw = ref_price_ + (buy ? in.protect_points : -in.protect_points);
    const double limit = std::round(raw / in.tick_size) * in.tick_size;
    if (!cfg_.xts.live_orders_enabled) {
      ELOG_WARN("live orders disabled: would {} {} lots @ {}", buy ? "BUY" : "SELL", lots, limit);
      return;
    }
    const auto uid = std::format("{}{}{:02}", kOrderPrefix, now_ms(), ++seq_ % 100);  // 17 chars
    const auto body = std::format(
        R"({{"exchangeSegment":"{}","exchangeInstrumentID":{},"productType":"{}","orderType":"LIMIT",)"
        R"("orderSide":"{}","timeInForce":"DAY","disclosedQuantity":0,"orderQuantity":{},"limitPrice":{},)"
        R"("stopPrice":0,"orderUniqueIdentifier":"{}","clientID":"{}"}})",
        in.segment, in.id, in.product, buy ? "BUY" : "SELL", lots * unit_, limit, uid,
        json_escape(cfg_.xts.client_id));
    working_ = Working{make_order_id(uid), buy ? 1 : -1, lots, 0, now_ms()};
    redis_->set_fields("order:" + uid, {{"side", buy ? "BUY" : "SELL"},
                                        {"lots", std::to_string(lots)},
                                        {"limit", std::to_string(limit)},
                                        {"status", "PendingNew"},
                                        {"sent_ms", std::to_string(now_ms())}});
    try {
      const auto r = http_->call("POST", "/orders", body);
      if (!r.ok()) throw xts_error("place order", r);
      JsonReader json;
      const auto app_id = as_int(json.parse(r.body)["result"]["AppOrderID"]);
      redis_->set_fields("order:" + uid, {{"app_order_id", std::to_string(app_id)}});
      ELOG_INFO("placed {} {} {} lots @ {} -> AppOrderID {}", uid, buy ? "BUY" : "SELL", lots, limit, app_id);
    } catch (const std::exception& e) {
      ELOG_ERROR("order {} not placed: {}", uid, e.what());
      redis_->set_fields("order:" + uid, {{"status", "Rejected"}, {"reason", e.what()}});
      working_.reset();
      give_up();
    }
  }

  // ---------------------------------------------------------------- reconciliation
  void reconcile() {
    try {
      const auto r = http_->call("GET", "/portfolio/positions?dayOrNet=NetWise");
      if (!r.ok()) {
        ELOG_WARN("reconcile: HTTP {} {}", r.status, r.body.substr(0, 200));
        return;
      }
      const auto broker = broker_lots(r.body);
      load_position();
      const char* status = "match";
      if (working_) {
        status = "skipped (order working)";
        if (!working_->stale_warned && now_ms() - working_->sent_ms > cfg_.engine.order_engine.stale_order_ms) {
          working_->stale_warned = true;
          ELOG_WARN("order {} has no final update after {} s; check the XTS order book", view(working_->id),
                    cfg_.engine.order_engine.stale_order_ms / 1000);
        }
      } else if (broker != position_) {
        status = "mismatch: broker adopted";
        ELOG_WARN("position mismatch: broker {} lots, Redis {} lots; adopting broker", broker, position_);
        position_ = target_ = broker;
        save_position("reconcile");
      }
      redis_->set_fields("recon", {{"broker_lots", std::to_string(broker)},
                                   {"redis_lots", std::to_string(position_)},
                                   {"status", status},
                                   {"checked_ms", std::to_string(now_ms())}});
      ELOG_DEBUG("reconcile: broker {} lots, Redis {} lots, {}", broker, position_, status);
    } catch (const std::exception& e) {
      ELOG_WARN("reconcile: {}", e.what());
    }
  }

  // Net lots for this instrument + product in a /portfolio/positions reply (0 when no row).
  std::int64_t broker_lots(const std::string& body) {
    const simdjson::dom::element doc = dom_.parse(body);
    simdjson::dom::array rows;
    if (doc["result"]["positionList"].get_array().get(rows) != simdjson::SUCCESS) return 0;
    std::int64_t units = 0;
    for (const simdjson::dom::element row : rows) {
      auto id = dom_number(row, "ExchangeInstrumentID");
      if (!id) id = dom_number(row, "ExchangeInstrumentId");
      if (!id || static_cast<std::int64_t>(*id) != cfg_.instrument.id) continue;
      std::string_view product;
      if (row["ProductType"].get_string().get(product) != simdjson::SUCCESS || product != cfg_.instrument.product)
        continue;
      auto qty = dom_number(row, "NetPosition");
      if (!qty) qty = dom_number(row, "Quantity");
      units += static_cast<std::int64_t>(qty.value_or(0.0));
    }
    return units / unit_;
  }

  const Config& cfg_;
  const XtsSession ia_;
  Queue<EngineEvent>& in_;
  const std::string pos_key_;
  const std::int64_t unit_;  // XTS quantity per lot
  RedisStore* redis_ = nullptr;
  RestClient* http_ = nullptr;
  simdjson::dom::parser dom_;
  std::int64_t position_ = 0, target_ = 0, seq_ = 0;
  double ref_price_ = 0;
  std::optional<Working> working_;
};

}  // namespace edge
