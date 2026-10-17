#pragma once
// libcurl JSON client. One instance per thread (not thread-safe). Built to keep one warm connection:
//  - the easy handle lives as long as the client, so the TCP+TLS connection is reused (keep-alive)
//  - TLS session IDs are cached on the handle (libcurl default), so a reconnect resumes TLS
//  - max_conn_age_s: libcurl would otherwise refuse to reuse a connection idle > 118 s
//  - TCP keepalive probes after tcp_keepidle_s, every tcp_keepintvl_s: a dead link is noticed fast
//  - DNS answers cached dns_cache_s; header list built once (rebuilt only by set_token); URL buffer reused
// All tuning comes from engine-config.json "rest". Callers that go quiet for long (the order engine)
// should call a cheap endpoint when idle_ms() grows, because the server may still close idle connections.
#include <curl/curl.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "edge/core/config.hpp"

namespace edge {

struct HttpResponse {
  long status = 0;
  long http_version = 0;  // CURL_HTTP_VERSION_* actually used
  std::string body;
  bool ok() const { return status >= 200 && status < 300; }
};

class RestClient {
 public:
  RestClient(std::string base, const Config::Engine::Rest& o) : base_(std::move(base)), curl_(curl_easy_init()) {
    if (!curl_) throw std::runtime_error("curl_easy_init failed");
    curl_easy_setopt(curl_, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl_, CURLOPT_TCP_KEEPIDLE, static_cast<long>(o.tcp_keepidle_s));
    curl_easy_setopt(curl_, CURLOPT_TCP_KEEPINTVL, static_cast<long>(o.tcp_keepintvl_s));
    curl_easy_setopt(curl_, CURLOPT_MAXAGE_CONN, static_cast<long>(o.max_conn_age_s));
    curl_easy_setopt(curl_, CURLOPT_DNS_CACHE_TIMEOUT, static_cast<long>(o.dns_cache_s));
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(o.connect_timeout_ms));
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, static_cast<long>(o.request_timeout_ms));
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &RestClient::write_cb);
    set_token({});
  }
  ~RestClient() {
    curl_easy_cleanup(curl_);
    curl_slist_free_all(headers_);
  }
  RestClient(const RestClient&) = delete;
  RestClient& operator=(const RestClient&) = delete;

  void set_token(std::string_view token) {
    curl_slist_free_all(headers_);
    headers_ = curl_slist_append(nullptr, "Content-Type: application/json");
    if (!token.empty()) headers_ = curl_slist_append(headers_, ("authorization: " + std::string(token)).c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
  }

  HttpResponse call(const char* method, std::string_view path, std::string_view body = {}) {
    HttpResponse r;
    r.body.reserve(4096);
    url_.assign(base_).append(path);
    curl_easy_setopt(curl_, CURLOPT_URL, url_.c_str());
    if (body.empty()) {
      curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
    } else {
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.data());
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method);  // after HTTPGET, which resets the verb
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &r.body);
    const CURLcode rc = curl_easy_perform(curl_);
    last_call_ = Clock::now();
    if (rc != CURLE_OK) throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc) + " " + url_);
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &r.status);
    curl_easy_getinfo(curl_, CURLINFO_HTTP_VERSION, &r.http_version);
    return r;
  }

  // Milliseconds since the last call finished (large before the first call).
  std::int64_t idle_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - last_call_).count();
  }

 private:
  using Clock = std::chrono::steady_clock;

  static size_t write_cb(char* p, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, size * n);
    return size * n;
  }

  std::string base_, url_;
  CURL* curl_;
  curl_slist* headers_ = nullptr;
  Clock::time_point last_call_{};
};

}  // namespace edge
