#pragma once
// XTS sessions and socket URLs.
// Market Data: POST /apimarketdata/auth/login with the app key/secret from Redis. One live session per
//   key: logging in here ends any other process's session on the same key.
// Interactive: the daily token + userID come from Redis (written by the OAuth login step).
#include <format>
#include <stdexcept>
#include <string>

#include "edge/core/config.hpp"
#include "edge/core/json.hpp"
#include "edge/store/redis_store.hpp"
#include "edge/net/rest_client.hpp"

namespace edge {

struct XtsSession {
  std::string token, user_id;
};

inline std::runtime_error xts_error(std::string_view what, const HttpResponse& r) {
  return std::runtime_error(std::format("{} failed: HTTP {} {}", what, r.status, r.body.substr(0, 300)));
}

inline XtsSession md_login(RestClient& http, RedisStore& redis, const Config::Redis& keys) {
  const auto body = std::format(R"({{"appKey":"{}","secretKey":"{}","source":"WEBAPI"}})",
                                json_escape(redis.required(keys.md_app_key)),
                                json_escape(redis.required(keys.md_secret_key)));
  const auto r = http.call("POST", "/auth/login", body);
  if (!r.ok()) throw xts_error("market data login", r);
  JsonReader json;
  auto result = json.parse(r.body)["result"];
  XtsSession s;
  s.token = std::string_view(result["token"]);
  s.user_id = std::string_view(result["userID"]);
  return s;
}

inline XtsSession ia_session(RedisStore& redis, const Config::Redis& keys) {
  return {redis.required(keys.ia_token), redis.required(keys.ia_user_id)};
}

inline std::string md_socket_target(const XtsSession& s) {
  return std::format("/apimarketdata/socket.io/?token={}&userID={}&publishFormat=JSON&broadcastMode=Full"
                     "&EIO=3&transport=websocket", s.token, s.user_id);
}

inline std::string ia_socket_target(const XtsSession& s) {
  return std::format("/interactive/socket.io/?token={}&userID={}&apiType=INTERACTIVE&EIO=3&transport=websocket",
                     s.token, s.user_id);
}

}  // namespace edge
