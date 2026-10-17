#pragma once
// Socket.IO v2 (Engine.IO v3) client over IXWebSocket (WSS), as XTS speaks it.
// Engine.IO: '0' open {pingInterval,pingTimeout}, '2' ping, '3' pong, '4' message.
//   The client pings every pingInterval; XTS also pings the client (Interactive does, ~10 s
//   timeout) and drops the socket unless each '2' is answered with '3'.
// Socket.IO inside '4': '0' connect, '1' disconnect, '2' event ["name", data], '4' error.
// IXWebSocket reads on its own internal thread and calls on_event/on_connect from there.
// run() blocks the calling thread, which owns the client ping and the dead-link watchdog;
// IXWebSocket reconnects automatically (1s..30s backoff) until stop is requested.
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>

#include "edge/core/json.hpp"
#include "edge/core/log.hpp"

namespace edge {

class SocketIoClient {
 public:
  using EventFn = std::function<void(std::string_view name, std::string_view data)>;
  using ConnectFn = std::function<void()>;

  // target: path + query, e.g. "/apimarketdata/socket.io/?token=..&EIO=3&transport=websocket".
  SocketIoClient(std::string label, const std::string& host, const std::string& target, EventFn on_event,
                 ConnectFn on_connect)
      : label_(std::move(label)), on_event_(std::move(on_event)), on_connect_(std::move(on_connect)) {
    ix::initNetSystem();
    ws_.setUrl("wss://" + host + target);
    ws_.disablePerMessageDeflate();
    ws_.enableAutomaticReconnection();
    ws_.setMinWaitBetweenReconnectionRetries(1000);
    ws_.setMaxWaitBetweenReconnectionRetries(30000);
    ix::SocketTLSOptions tls;
    tls.caFile = "SYSTEM";
    ws_.setTLSOptions(tls);
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& m) { on_message(m); });
  }

  void run(std::stop_token st) {
    ws_.start();
    std::mutex m;
    std::condition_variable_any cv;
    while (!st.stop_requested()) {
      {
        std::unique_lock lk(m);
        cv.wait_for(lk, st, std::chrono::milliseconds(250), [] { return false; });
      }
      if (st.stop_requested() || !open_.load()) continue;
      const auto now = steady_ms();
      if (now - last_rx_.load() > ping_every_.load() + ping_timeout_.load()) {
        ELOG_WARN("{} socket silent for pingInterval + pingTimeout; dropping", label_);
        open_.store(false);
        ws_.close();  // automatic reconnection takes over
      } else if (now >= next_ping_.load()) {
        ws_.sendText("2");
        next_ping_.store(now + ping_every_.load());
      }
    }
    ws_.stop();
    ELOG_INFO("{} socket stopped", label_);
  }

 private:
  static std::int64_t steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  }

  // Runs on the IXWebSocket thread.
  void on_message(const ix::WebSocketMessagePtr& m) {
    switch (m->type) {
      case ix::WebSocketMessageType::Message:
        last_rx_.store(steady_ms());
        try {
          on_frame(m->str);
        } catch (const std::exception& e) {
          ELOG_WARN("{} frame: {}", label_, e.what());
        }
        break;
      case ix::WebSocketMessageType::Open:
        last_rx_.store(steady_ms());
        ELOG_INFO("{} socket upgraded", label_);
        break;
      case ix::WebSocketMessageType::Close:
        open_.store(false);
        ELOG_WARN("{} socket closed: {} {}", label_, m->closeInfo.code, m->closeInfo.reason);
        break;
      case ix::WebSocketMessageType::Error:
        open_.store(false);
        ELOG_WARN("{} socket error: {} (retry {}, HTTP {})", label_, m->errorInfo.reason, m->errorInfo.retries,
                  m->errorInfo.http_status);
        break;
      default:  // Ping / Pong / Fragment
        break;
    }
  }

  void on_frame(std::string_view f) {
    if (f.empty()) return;
    const char sio = f.size() > 1 ? f[1] : '\0';
    switch (f[0]) {
      case '0': {
        auto& d = json_.parse(f.substr(1));
        ping_every_.store(int64_t(d["pingInterval"]));
        ping_timeout_.store(int64_t(d["pingTimeout"]));
        next_ping_.store(steady_ms() + ping_every_.load());
        open_.store(true);
        break;
      }
      case '2':  // server ping: answer with pong + same payload
        ws_.sendText("3" + std::string(f.substr(1)));
        break;
      case '4':
        if (sio == '2') {
          dispatch(f.substr(2));
        } else if (sio == '0') {
          ELOG_INFO("{} socket connected", label_);
          on_connect_();
        } else if (sio == '1') {
          ELOG_WARN("{} server disconnected the socket", label_);
        } else if (sio == '4') {
          ELOG_WARN("{} socket error: {}", label_, f.substr(2));
        }
        break;
      case '1':
        ELOG_WARN("{} engine.io close", label_);
        break;
      default:  // '3' pong: last_rx_ is already refreshed
        break;
    }
  }

  // payload: ["event-name", data]; XTS sends data as a JSON string (unescaped here) or an object.
  void dispatch(std::string_view payload) {
    auto arr = json_.parse(payload).get_array();
    auto it = arr.begin();
    const std::string_view name = (*it).get_string();
    ++it;
    simdjson::ondemand::value v = *it;
    const std::string_view data =
        v.type() == simdjson::ondemand::json_type::string ? v.get_string().value() : v.raw_json().value();
    on_event_(name, data);
  }

  std::string label_;
  EventFn on_event_;
  ConnectFn on_connect_;
  JsonReader json_;  // IXWebSocket thread only
  std::atomic<bool> open_{false};
  std::atomic<std::int64_t> last_rx_{0}, next_ping_{0}, ping_every_{25000}, ping_timeout_{20000};
  ix::WebSocket ws_;  // last: destroyed first, so its thread never sees the members above destroyed
};

}  // namespace edge
