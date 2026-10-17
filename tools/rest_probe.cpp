// rest_probe v1.3: exercises edge::RestClient (libcurl) against the XTS Interactive REST API.
// Read-only: GET /user/profile and GET /user/balance, each twice, to show the first-call cost
// (TCP + TLS) and the reused keep-alive connection. Calls are 1.1 s apart: XTS allows 1 call/s per
// Interactive endpoint (HTTP 429 e-apirl-0004 otherwise). Uses today's session token from Redis.
// With idle_seconds, it then waits that long and times one more call: shows whether the connection
// survived the idle gap (a few ms) or had to be rebuilt (TCP + TLS again).
// Usage: rest_probe [config/config.json] [idle_seconds]   (load the Redis password first, as for run.sh)
#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

#include "edge/core/config.hpp"
#include "edge/core/json.hpp"
#include "edge/net/rest_client.hpp"
#include "edge/store/redis_store.hpp"
#include "edge/xts/session.hpp"

int main(int argc, char** argv) {
  using namespace edge;
  try {
    const Config cfg = load_config(argc > 1 ? argv[1] : "config/config.json");
    const int idle_s = argc > 2 ? std::atoi(argv[2]) : 0;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    RedisStore redis(cfg.redis);
    const auto ia = ia_session(redis, cfg.redis);
    RestClient http(cfg.xts.ia_base(), cfg.engine.rest);
    http.set_token(ia.token);
    JsonReader json;
    const std::string balance_path = "/user/balance?clientID=" + cfg.xts.client_id;

    auto timed = [&](const char* label, const std::string& path, std::chrono::milliseconds gap) {
      std::this_thread::sleep_for(gap);
      const auto t0 = std::chrono::steady_clock::now();
      const auto r = http.call("GET", path);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      std::printf("%-26s HTTP %ld  %7.1f ms  %zu bytes\n", label, r.status, ms, r.body.size());
      std::fflush(stdout);
      if (!r.ok()) throw xts_error(label, r);
      return r;
    };
    constexpr std::chrono::milliseconds kGap{1100};

    const auto p = timed("profile (new conn)", "/user/profile", {});
    std::printf("  client %s, HTTP/%s\n", std::string(std::string_view(json.parse(p.body)["result"]["ClientId"])).c_str(),
                p.http_version == CURL_HTTP_VERSION_2_0 ? "2" : p.http_version == CURL_HTTP_VERSION_3 ? "3" : "1.1");
    timed("profile (keep-alive)", "/user/profile", kGap);
    const auto b = timed("balance", balance_path, kGap);
    auto rms = json.parse(b.body)["result"]["BalanceList"].at(0)["limitObject"]["RMSSubLimits"];
    std::printf("  net margin available: %.2f\n", as_double(rms["netMarginAvailable"]));
    timed("balance (again)", balance_path, kGap);

    if (idle_s > 0) {
      std::printf("idle %d s...\n", idle_s);
      std::fflush(stdout);
      const std::string label = "balance after " + std::to_string(idle_s) + " s idle";
      timed(label.c_str(), balance_path, std::chrono::seconds(idle_s));
    }
    curl_global_cleanup();
    std::puts("rest_probe: OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "rest_probe: FAILED: %s\n", e.what());
    return 1;
  }
}
