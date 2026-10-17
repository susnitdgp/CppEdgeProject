// rest_probe v1.1: exercises edge::RestClient (libcurl) against the XTS Interactive REST API.
// Read-only: GET /user/profile and GET /user/balance, each twice, to show the first-call cost
// (TCP + TLS) and the reused keep-alive connection. Calls are 1.1 s apart: XTS allows 1 call/s per
// Interactive endpoint (HTTP 429 e-apirl-0004 otherwise). Uses today's session token from Redis.
// Usage: rest_probe [config/config.json]   (load the Redis password first, as for run.sh)
#include <curl/curl.h>

#include <chrono>
#include <utility>
#include <cstdio>
#include <string>
#include <thread>

#include "edge/core/config.hpp"
#include "edge/core/json.hpp"
#include "edge/net/rest_client.hpp"
#include "edge/store/redis_store.hpp"
#include "edge/xts/session.hpp"

int main(int argc, char** argv) {
  using namespace edge;
  try {
    const Config cfg = load_config(argc > 1 ? argv[1] : "config/config.json");
    curl_global_init(CURL_GLOBAL_DEFAULT);
    RedisStore redis(cfg.redis);
    const auto ia = ia_session(redis, cfg.redis);
    RestClient http(cfg.xts.ia_base());
    http.set_token(ia.token);
    JsonReader json;

    auto timed = [&](const char* label, const std::string& path) {
      static bool first = true;
      if (!std::exchange(first, false)) std::this_thread::sleep_for(std::chrono::milliseconds(1100));
      const auto t0 = std::chrono::steady_clock::now();
      const auto r = http.call("GET", path);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      std::printf("%-22s HTTP %ld  %7.1f ms  %zu bytes\n", label, r.status, ms, r.body.size());
      if (!r.ok()) throw xts_error(label, r);
      return r;
    };

    for (int i = 1; i <= 2; ++i) {
      const auto p = timed(i == 1 ? "profile (new conn)" : "profile (keep-alive)", "/user/profile");
      if (i == 1) {
        auto res = json.parse(p.body)["result"];
        std::printf("  client %s\n", std::string(std::string_view(res["ClientId"])).c_str());
      }
    }
    for (int i = 1; i <= 2; ++i) {
      const auto b = timed(i == 1 ? "balance" : "balance (again)", "/user/balance?clientID=" + cfg.xts.client_id);
      if (i == 1) {
        auto rms = json.parse(b.body)["result"]["BalanceList"].at(0)["limitObject"]["RMSSubLimits"];
        std::printf("  net margin available: %.2f\n", as_double(rms["netMarginAvailable"]));
      }
    }
    curl_global_cleanup();
    std::puts("rest_probe: OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "rest_probe: FAILED: %s\n", e.what());
    return 1;
  }
}
