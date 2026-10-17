#pragma once
// libcurl JSON client. One instance per thread (not thread-safe); keeps its TLS connection alive.
#include <curl/curl.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace edge {

struct HttpResponse {
  long status = 0;
  std::string body;
  bool ok() const { return status >= 200 && status < 300; }
};

class RestClient {
 public:
  explicit RestClient(std::string base) : base_(std::move(base)), curl_(curl_easy_init()) {
    if (!curl_) throw std::runtime_error("curl_easy_init failed");
    curl_easy_setopt(curl_, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &RestClient::write_cb);
  }
  ~RestClient() { curl_easy_cleanup(curl_); }
  RestClient(const RestClient&) = delete;
  RestClient& operator=(const RestClient&) = delete;

  void set_token(std::string token) { token_ = "authorization: " + token; }

  HttpResponse call(const char* method, std::string_view path, std::string_view body = {}) {
    HttpResponse r;
    const std::string url = base_ + std::string(path);
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    if (!token_.empty()) headers = curl_slist_append(headers, token_.c_str());
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    if (body.empty()) {
      curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
    } else {
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.data());
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method);   // after HTTPGET, which resets the verb
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &r.body);
    const CURLcode rc = curl_easy_perform(curl_);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc) + " " + url);
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &r.status);
    return r;
  }

 private:
  static size_t write_cb(char* p, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, size * n);
    return size * n;
  }

  std::string base_, token_;
  CURL* curl_;
};

}  // namespace edge
