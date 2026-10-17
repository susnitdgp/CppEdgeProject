#pragma once
// Plain data passed between the five threads, the queue type, and small shared helpers.
#include <blockingconcurrentqueue.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>

namespace edge {

template <class T>
using Queue = moodycamel::BlockingConcurrentQueue<T>;


struct Tick {                       // one trade (TotalTradedQuantity moved)
  std::int64_t instrument_id;
  std::int64_t exch_ts_ms;          // Unix UTC ms
  double price;
  std::int64_t qty;                 // TotalTradedQuantity delta
};

struct Candle {
  std::int64_t instrument_id;
  std::int64_t open_ts_ms;          // bucket start, Unix UTC ms
  double open, high, low, close;
  std::int64_t volume;
};

struct Signal {                     // target position, in strategy units (+1 long, -1 short, 0 flat)
  std::int64_t instrument_id;
  std::int32_t target;
  double ref_price;
  std::int64_t candle_ts_ms;
};

enum class OrderStatus : std::uint8_t { PendingNew, New, PartiallyFilled, Filled, Cancelled, Rejected, Unknown };

using OrderId = std::array<char, 21>;   // XTS orderUniqueIdentifier: max 20 chars + NUL
inline constexpr std::string_view kOrderPrefix = "CE";   // marks this bot's orders on a shared account

struct OrderUpdate {
  OrderId unique_id;
  std::int64_t app_order_id;
  OrderStatus status;
  std::int64_t cum_qty;
  std::int64_t leaves_qty;
  double avg_price;
};

using EngineEvent = std::variant<Signal, OrderUpdate>;   // T3 + T5 -> T4

struct RawEvent {                   // T1 -> T6 recorder: one market-data message as received
  std::int64_t rx_ns;               // our receive time, Unix ns
  std::string name;                 // XTS event name ("1501-json-full") or "socket"
  std::string data;                 // JSON payload as sent, or a quoted socket status string
};

inline std::int64_t now_ns() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

inline OrderStatus parse_status(std::string_view s) {
  if (s == "New" || s == "Open") return OrderStatus::New;
  if (s == "PartiallyFilled") return OrderStatus::PartiallyFilled;
  if (s == "Filled") return OrderStatus::Filled;
  if (s == "Cancelled") return OrderStatus::Cancelled;
  if (s == "Rejected") return OrderStatus::Rejected;
  if (s == "PendingNew") return OrderStatus::PendingNew;
  return OrderStatus::Unknown;
}

inline std::string_view to_string(OrderStatus s) {
  static constexpr std::string_view names[] = {"PendingNew", "New", "PartiallyFilled", "Filled",
                                               "Cancelled", "Rejected", "Unknown"};
  return names[static_cast<std::size_t>(s)];
}

inline bool is_terminal(OrderStatus s) {
  return s == OrderStatus::Filled || s == OrderStatus::Cancelled || s == OrderStatus::Rejected;
}

inline OrderId make_order_id(std::string_view s) {
  OrderId id{};
  std::memcpy(id.data(), s.data(), std::min(s.size(), id.size() - 1));
  return id;
}

inline std::string_view view(const OrderId& id) { return {id.data(), std::strlen(id.data())}; }

// XTS times are IST seconds, counted from 1970 (current feeds) or 1980 (Symphony doc examples).
inline std::int64_t xts_to_unix_ms(std::int64_t xts) {
  constexpr std::int64_t k1980 = 315'532'800, kLimit = 1'500'000'000, kIst = 19'800;
  if (xts <= 0) return 0;
  return ((xts < kLimit ? xts + k1980 : xts) - kIst) * 1000;
}

inline std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

inline void set_thread_name(const char* name) { pthread_setname_np(pthread_self(), name); }

}  // namespace edge
