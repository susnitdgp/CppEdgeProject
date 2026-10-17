#pragma once
// simdjson helpers. JsonReader: one per thread; views it returns live until its next parse().
// XTS sends some numbers as text ("8,818.00"); the readers below accept both forms.
#include <simdjson.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

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

// "8,818.00" -> 8818.0; nullopt when the text is not a number.
inline std::optional<double> parse_number_text(std::string_view s) {
  std::string clean;
  clean.reserve(s.size());
  for (char c : s) if (c != ',') clean.push_back(c);
  double d = 0;
  if (std::from_chars(clean.data(), clean.data() + clean.size(), d).ec != std::errc{}) return std::nullopt;
  return d;
}

// On-Demand value -> double (number or numeric text; unparsable text -> 0).
inline double as_double(simdjson::ondemand::value v) {
  if (v.type() != simdjson::ondemand::json_type::string) return v.get_double();
  return parse_number_text(v.get_string().value()).value_or(0.0);
}

// On-Demand value -> integer (integer, float or numeric text).
inline std::int64_t as_int(simdjson::ondemand::value v) {
  if (v.type() == simdjson::ondemand::json_type::string)
    return static_cast<std::int64_t>(parse_number_text(v.get_string().value()).value_or(0.0));
  if (v.get_number_type() == simdjson::ondemand::number_type::floating_point_number)
    return static_cast<std::int64_t>(v.get_double().value());
  return v.get_int64().value();
}

// DOM lookup: a number (or numeric text) at `key`, or nullopt when absent or not numeric.
inline std::optional<double> dom_number(simdjson::dom::element obj, std::string_view key) {
  auto f = obj[key];
  if (f.error()) return std::nullopt;
  const simdjson::dom::element v = f.value();
  if (v.is_number()) return v.get_double().value();
  if (v.is_string()) return parse_number_text(v.get_string().value());
  return std::nullopt;
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
