#pragma once
// simdjson On-Demand helpers. One JsonReader per thread; views it returns live until its next parse().
#include <simdjson.h>

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace edge {

class JsonReader {
 public:
  simdjson::ondemand::document& parse(std::string_view s) {
    buf_.reserve(s.size() + simdjson::SIMDJSON_PADDING);
    buf_.assign(s);
    doc_ = parser_.iterate(buf_.data(), buf_.size(), buf_.capacity());
    return doc_;
  }

 private:
  simdjson::ondemand::parser parser_;
  std::string buf_;
  simdjson::ondemand::document doc_;
};

// XTS sends some numbers as text ("8,818.00"); accept both.
inline double as_double(simdjson::ondemand::value v) {
  if (v.type() != simdjson::ondemand::json_type::string) return v.get_double();
  std::string_view s = v.get_string();
  std::string clean;
  for (char c : s) if (c != ',') clean.push_back(c);
  double d = 0;
  std::from_chars(clean.data(), clean.data() + clean.size(), d);
  return d;
}

inline std::int64_t as_int(simdjson::ondemand::value v) {
  if (v.type() == simdjson::ondemand::json_type::string) {
    std::string_view s = v.get_string();
    std::int64_t i = 0;
    std::from_chars(s.data(), s.data() + s.size(), i);
    return i;
  }
  if (v.get_number_type() == simdjson::ondemand::number_type::floating_point_number)
    return static_cast<std::int64_t>(v.get_double().value());
  return v.get_int64().value();
}

// Escape a value for embedding in a JSON string literal (ids, symbols: no control chars expected).
inline std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

}  // namespace edge
