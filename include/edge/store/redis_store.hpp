#pragma once
// Redis access: credentials in, bot state out. Keys under Config::Redis::state_prefix:
//   <p>order:<uniqueId>   hash  order state (engine writes PendingNew, update thread the rest)
//   <p>position:<instId>  hash  lots, avg_price, updated_ms (engine)
//   <p>trades             list  raw XTS trade events (update thread)
#include <sw/redis++/redis++.h>

#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "edge/core/config.hpp"

namespace edge {

class RedisStore {
 public:
  explicit RedisStore(const Config::Redis& c) : redis_(options(c)), prefix_(c.state_prefix) {}

  std::string required(const std::string& key) {
    auto v = redis_.get(key);
    if (!v || v->empty()) throw std::runtime_error("redis key missing: " + key);
    return *v;
  }

  std::string get_field(std::string_view key, std::string_view field) {
    auto v = redis_.hget(prefix_ + std::string(key), field);
    return v ? *v : std::string{};
  }

  void set_fields(std::string_view key, std::initializer_list<std::pair<std::string_view, std::string>> f) {
    redis_.hset(prefix_ + std::string(key), f.begin(), f.end());
  }

  void push(std::string_view list, std::string_view value) { redis_.lpush(prefix_ + std::string(list), value); }

 private:
  static sw::redis::ConnectionOptions options(const Config::Redis& c) {
    sw::redis::ConnectionOptions o;
    o.host = c.host;
    o.port = static_cast<int>(c.port);
    o.db = static_cast<int>(c.db);
    if (const char* pw = std::getenv(c.password_env.c_str())) o.password = pw;
    o.socket_timeout = std::chrono::milliseconds(500);
    return o;
  }

  sw::redis::Redis redis_;
  std::string prefix_;
};

}  // namespace edge
