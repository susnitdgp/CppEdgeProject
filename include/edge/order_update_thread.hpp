#pragma once
// Thread 5: Interactive socket -> Redis order state + OrderUpdate to the engine.
// Only this bot's orders (orderUniqueIdentifier starts with kOrderPrefix) are tracked; the account
// may be shared with other bots. Trade events are appended raw to the <p>trades list.
#include <stop_token>

#include "edge/config.hpp"
#include "edge/json.hpp"
#include "edge/log.hpp"
#include "edge/redis_store.hpp"
#include "edge/socketio_client.hpp"
#include "edge/types.hpp"
#include "edge/xts.hpp"

namespace edge {

class OrderUpdateThread {
 public:
  OrderUpdateThread(const Config& cfg, XtsSession ia, Queue<EngineEvent>& out)
      : cfg_(cfg), ia_(std::move(ia)), out_(out) {}

  void operator()(std::stop_token st) {
    set_thread_name("edge-ordupd");
    try {
      RedisStore redis(cfg_.redis);
      redis_ = &redis;
      SocketIoClient socket(
          "interactive", cfg_.xts.host, ia_socket_target(ia_),
          [this](std::string_view name, std::string_view data) { on_event(name, data); },
          [] {});
      socket.run(st);
    } catch (const std::exception& e) {
      ELOG_ERROR("order update thread stopped: {}", e.what());
    }
  }

 private:
  void on_event(std::string_view name, std::string_view data) {
    if (name == "order") {
      on_order(data);
    } else if (name == "trade") {
      redis_->push("trades", data);
    } else if (name == "joined") {
      ELOG_INFO("interactive socket joined");
    } else if (name == "logout" || name == "error") {
      ELOG_WARN("interactive {}: {}", name, data);
    }
  }

  void on_order(std::string_view data) {
    auto& d = json_.parse(data);
    const std::string_view uid = d["OrderUniqueIdentifier"].get_string();
    if (!uid.starts_with(kOrderPrefix)) return;
    OrderUpdate u{make_order_id(uid),
                  as_int(d["AppOrderID"]),
                  parse_status(d["OrderStatus"].get_string()),
                  as_int(d["CumulativeQuantity"]),
                  as_int(d["LeavesQuantity"]),
                  as_double(d["OrderAverageTradedPrice"])};
    auto reason_field = d["CancelRejectReason"];   // optional
    const std::string_view reason = reason_field.error() ? std::string_view{} : reason_field.get_string().value();
    redis_->set_fields(std::string("order:").append(uid),
                       {{"app_order_id", std::to_string(u.app_order_id)},
                        {"status", std::string(to_string(u.status))},
                        {"cum_qty", std::to_string(u.cum_qty)},
                        {"leaves_qty", std::to_string(u.leaves_qty)},
                        {"avg_price", std::to_string(u.avg_price)},
                        {"reason", std::string(reason)},
                        {"updated_ms", std::to_string(now_ms())}});
    ELOG_INFO("order {} {} cum {} avg {}", view(u.unique_id), to_string(u.status), u.cum_qty, u.avg_price);
    out_.enqueue(u);
  }

  const Config& cfg_;
  const XtsSession ia_;
  Queue<EngineEvent>& out_;
  RedisStore* redis_ = nullptr;
  JsonReader json_;
};

}  // namespace edge
