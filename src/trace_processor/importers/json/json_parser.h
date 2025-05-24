/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_JSON_JSON_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_JSON_JSON_PARSER_H_

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/importers/json/json_utils.h"

namespace perfetto::trace_processor::json {

enum class JsonParseError : uint8_t {
  kSuccess = 0,
  kEmptyInput,
  kInvalidStartChar,
  kUnterminatedString,
  kInvalidEscapeSequence,
  kUnterminatedObject,
  kUnterminatedArray,
  kInvalidLiteral,
  kMalformedNumberToken,  // Lexical/scan error for numbers
  kLiteralTooShort,
  kMissingColon,
  kMissingCommaOrTerminator,  // Expected comma or end of structure
  kNumberConversion,  // Semantic error converting number string (e.g. overflow)
  kKeyNotString       // Object key was not a JSON string type
};

struct JsonEmpty {};
struct JsonNull {};
struct SimpleJsonString {
  std::string_view data;  // Content, no quotes
};
struct ComplexJsonString {
  std::string data;  // Owned, unescaped content
};
struct JsonObject {
  std::string_view raw_data;  // Includes {}
};
struct JsonArray {
  std::string_view raw_data;  // Includes []
};
using JsonValue = std::variant<JsonEmpty,
                               bool,
                               int64_t,
                               double,
                               JsonNull,
                               SimpleJsonString,
                               ComplexJsonString,
                               JsonObject,
                               JsonArray>;

namespace internal {

inline void SkipWhitespace(std::string_view& sv) {
  size_t pos = 0;
  while (pos < sv.length() && (sv[pos] == ' ' || sv[pos] == '\t' ||
                               sv[pos] == '\n' || sv[pos] == '\r')) {
    pos++;
  }
  sv.remove_prefix(pos);
}

inline JsonParseError ScanToEndOfString(std::string_view sv,
                                        size_t& out_length) {
  out_length = 1;
  while (out_length < sv.length()) {
    char c = sv[out_length];
    if (c == '\\') {
      out_length++;
      if (out_length >= sv.length()) {
        return JsonParseError::kUnterminatedString;
      }
      char esc_char = sv[out_length];
      switch (esc_char) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
          out_length++;
          break;
        case 'u':
          out_length++;
          if (out_length + 3 >= sv.length()) {
            return JsonParseError::kInvalidEscapeSequence;
          }
          for (int i = 0; i < 4; ++i) {
            if (!std::isxdigit(sv[out_length])) {
              return JsonParseError::kInvalidEscapeSequence;
            }
            out_length++;
          }
          break;
        default:
          return JsonParseError::kInvalidEscapeSequence;
      }
    } else if (c == '"') {
      out_length++;
      return JsonParseError::kSuccess;
    } else if (c < 0x20)
      return JsonParseError::kInvalidEscapeSequence;
    else {
      out_length++;
    }
  }
  return JsonParseError::kUnterminatedString;
}

inline JsonParseError PerformUnescaping(std::string_view str,
                                        std::string& res) {
  res.reserve(str.length());
  for (size_t i = 0; i < str.length(); ++i) {
    if (str[i] == '\\') {
      if (++i >= str.length()) {
        return JsonParseError::kInvalidEscapeSequence;
      }
      switch (str[i]) {
        case '"':
          res += '"';
          break;
        case '\\':
          res += '\\';
          break;
        case '/':
          res += '/';
          break;
        case 'b':
          res += '\b';
          break;
        case 'f':
          res += '\f';
          break;
        case 'n':
          res += '\n';
          break;
        case 'r':
          res += '\r';
          break;
        case 't':
          res += '\t';
          break;
        case 'u': {
          if (i + 4 >= str.length()) {
            return JsonParseError::kInvalidEscapeSequence;
          }
          uint32_t cp = 0;
          for (int j = 0; j < 4; ++j) {
            char hex = str[++i];
            cp <<= 4;
            if (hex >= '0' && hex <= '9') {
              cp += static_cast<uint32_t>(hex - '0');
            } else if (hex >= 'a' && hex <= 'f') {
              cp += static_cast<uint32_t>(hex - 'a' + 10);
            } else if (hex >= 'A' && hex <= 'F') {
              cp += static_cast<uint32_t>(hex - 'A' + 10);
            } else {
              return JsonParseError::kInvalidEscapeSequence;
            }
          }
          if (cp <= 0x7F) {
            res += static_cast<char>(cp);
          } else if (cp <= 0x7FF) {
            res += static_cast<char>(0xC0 | (cp >> 6));
            res += static_cast<char>(0x80 | (cp & 0x3F));
          } else if (cp <= 0xFFFF) {
            if (cp >= 0xD800 && cp <= 0xDFFF) {
              return JsonParseError::kInvalidEscapeSequence;
            }
            res += static_cast<char>(0xE0 | (cp >> 12));
            res += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            res += static_cast<char>(0x80 | (cp & 0x3F));
          } else {
            return JsonParseError::kInvalidEscapeSequence;
          }
          break;
        }
        default:
          return JsonParseError::kInvalidEscapeSequence;
      }
    } else {
      res += str[i];
    }
  }
  return JsonParseError::kSuccess;
}

inline JsonParseError ScanToEndOfDelimitedBlock(std::string_view sv,
                                                char open_delim,
                                                char close_delim,
                                                size_t& out_length) {
  out_length = 1;
  uint32_t bal = 1;
  while (out_length < sv.length() && bal > 0) {
    char c = sv[out_length];
    if (c == '"') {
      size_t str_len = 0;
      if (JsonParseError e = ScanToEndOfString(sv.substr(out_length), str_len);
          e != JsonParseError::kSuccess) {
        return e;
      }
      out_length += str_len;
    } else if (c == open_delim) {
      bal++;
      out_length++;
    } else if (c == close_delim) {
      bal--;
      out_length++;
    } else {
      out_length++;
    }
  }
  if (PERFETTO_LIKELY(bal == 0)) {
    return JsonParseError::kSuccess;
  }
  return open_delim == '{' ? JsonParseError::kUnterminatedObject
                           : JsonParseError::kUnterminatedArray;
}

inline bool StringToInt64(std::string_view sv, int64_t& out) {
  PERFETTO_DCHECK(!sv.empty());
  out = 0;

  bool negative = false;
  size_t pos = 0;
  if (sv[pos] == '-') {
    negative = true;
    pos++;
  }
  PERFETTO_DCHECK(pos < sv.length());

  const int64_t kAbsMaxDiv10 = std::numeric_limits<int64_t>::max() / 10;
  const int kAbsMaxMod10 = std::numeric_limits<int64_t>::max() % 10;
  for (; pos < sv.length(); ++pos) {
    PERFETTO_DCHECK(std::isdigit(sv[pos]));
    int digit = sv[pos] - '0';
    if (out > kAbsMaxDiv10 || (out == kAbsMaxDiv10 && digit > kAbsMaxMod10)) {
      // Special case for INT64_MIN
      if (negative && out == kAbsMaxDiv10 && digit == kAbsMaxMod10 + 1) {
        // This will be -(INT64_MAX/10 * 10 + (INT64_MAX%10 + 1)) which is
        // INT64_MIN
      } else {
        return false;
      }
    }
    out = out * 10 + digit;
  }
  if (negative) {
    out = -out;
  }
  return true;
}

inline bool StringToDouble(std::string_view sv, double& out) {
  out = 0.0;
  PERFETTO_DCHECK(!sv.empty());

  bool negative = false;
  size_t pos = 0;
  if (sv[pos] == '-') {
    negative = true;
    pos++;
  }
  PERFETTO_DCHECK(pos < sv.length());

  while (pos < sv.length() && std::isdigit(sv[pos])) {
    out = out * 10.0 + (sv[pos++] - '0');
  }
  if (pos < sv.length() && sv[pos] == '.') {
    pos++;
    double frac = 0.0;
    double div = 1.0;
    while (pos < sv.length() && std::isdigit(sv[pos])) {
      frac = frac * 10.0 + (sv[pos++] - '0');
      div *= 10.0;
    }
    if (div > 1.0) {
      out += frac / div;  // Add frac part only if there were digits
    }
  }
  if (pos < sv.length() && (sv[pos] == 'e' || sv[pos] == 'E')) {
    pos++;
    bool exp_neg = false;
    double exponent = 0;
    if (pos < sv.length() && (sv[pos] == '+' || sv[pos] == '-')) {
      if (sv[pos] == '-') {
        exp_neg = true;
      }
      pos++;
    }
    PERFETTO_DCHECK(pos < sv.length());
    size_t exp_digit_start = pos;
    while (pos < sv.length() && std::isdigit(sv[pos])) {
      exponent = exponent * 10 + (sv[pos++] - '0');
    }
    PERFETTO_DCHECK(pos != exp_digit_start);
    if (exp_neg) {
      out /= std::pow(10.0, exponent);
    } else {
      out *= std::pow(10.0, exponent);
    }
  }
  // Overflow/underflow from pow
  if (std::isinf(out) || std::isnan(out)) {
    return false;
  }
  out = negative ? -out : out;
  return true;
}

inline JsonParseError ParseNumber(std::string_view in,
                                  JsonValue& out_num,
                                  size_t& value_len) {
  std::string_view pos = in;
  if (pos.empty()) {
    return JsonParseError::kMalformedNumberToken;
  }

  bool is_float_like = false;
  if (pos.front() == '-') {
    pos.remove_prefix(1);
  }
  if (pos.empty()) {
    return JsonParseError::kMalformedNumberToken;
  }
  if (!pos.empty() && pos.front() == '0') {
    pos.remove_prefix(1);
    if (!pos.empty() && std::isdigit(pos.front())) {
      // "01" etc.
      return JsonParseError::kMalformedNumberToken;
    }
  } else if (!pos.empty() && pos.front() >= '1' && pos.front() <= '9') {
    pos.remove_prefix(1);
    while (!pos.empty() && std::isdigit(pos.front())) {
      pos.remove_prefix(1);
    }
  } else {
    return JsonParseError::kMalformedNumberToken;
  }

  if (!pos.empty() && pos.front() == '.') {
    is_float_like = true;
    pos.remove_prefix(1);
    std::string_view frac_start_pos = pos;
    while (!pos.empty() && std::isdigit(pos.front())) {
      pos.remove_prefix(1);
    }
    if (pos == frac_start_pos) {
      return JsonParseError::kMalformedNumberToken;
    }
  }

  if (!pos.empty() && (pos.front() == 'e' || pos.front() == 'E')) {
    is_float_like = true;
    pos.remove_prefix(1);
    if (!pos.empty() && (pos.front() == '+' || pos.front() == '-')) {
      pos.remove_prefix(1);
    }
    std::string_view exp_start_pos = pos;
    while (!pos.empty() && std::isdigit(pos.front())) {
      pos.remove_prefix(1);
    }
    if (pos == exp_start_pos) {
      return JsonParseError::kMalformedNumberToken;
    }
  }

  if (pos.size() == in.size() ||
      (in.front() == '-' && pos.size() == in.size() - 1)) {
    return JsonParseError::kMalformedNumberToken;
  }

  std::string_view num_token_sv = in.substr(0, in.size() - pos.size());
  if (is_float_like) {
    double d_val;
    if (StringToDouble(num_token_sv, d_val)) {
      out_num = d_val;
      value_len = num_token_sv.length();
      return JsonParseError::kSuccess;
    }
    return JsonParseError::kNumberConversion;
  }
  int64_t i_val;
  if (StringToInt64(num_token_sv, i_val)) {
    out_num = i_val;
    value_len = num_token_sv.length();
    return JsonParseError::kSuccess;
  }
  double d_val;
  if (StringToDouble(num_token_sv, d_val)) {
    out_num = d_val;
    value_len = num_token_sv.length();
    return JsonParseError::kSuccess;
  }
  return JsonParseError::kNumberConversion;
}

inline JsonParseError ParseJsonValue(std::string_view input_sv,
                                     JsonValue& out_value,
                                     std::string_view& out_sv) {
  out_value = JsonEmpty{};
  out_sv = input_sv;

  if (internal::SkipWhitespace(out_sv); out_sv.empty()) {
    return JsonParseError::kEmptyInput;
  }
  size_t value_len = 0;
  JsonParseError err = JsonParseError::kSuccess;
  switch (out_sv.front()) {
    case '{':
      err = internal::ScanToEndOfDelimitedBlock(out_sv, '{', '}', value_len);
      if (err == JsonParseError::kSuccess) {
        out_value = JsonObject{out_sv.substr(0, value_len)};
      }
      break;
    case '[':
      err = internal::ScanToEndOfDelimitedBlock(out_sv, '[', ']', value_len);
      if (err == JsonParseError::kSuccess) {
        out_value = JsonArray{out_sv.substr(0, value_len)};
      }
      break;
    case '"': {
      err = internal::ScanToEndOfString(out_sv, value_len);
      if (err != JsonParseError::kSuccess) {
        break;
      }
      // Remove the quotes.
      std::string_view str = out_sv.substr(1, value_len - 2);
      bool has_escape = false;
      for (char c : str) {
        if (c == '\\') {
          has_escape = true;
          break;
        }
      }
      if (has_escape) {
        std::string unescaped;
        err = internal::PerformUnescaping(str, unescaped);
        if (err == JsonParseError::kSuccess) {
          out_value = ComplexJsonString{std::move(unescaped)};
        }
      } else {
        out_value = SimpleJsonString{str};
      }
      break;
    }
    case 't':
      if (out_sv.length() >= 4 && out_sv.substr(0, 4) == "true") {
        value_len = 4;
        out_value = true;
      } else {
        err = (out_sv.length() < 4 ? JsonParseError::kLiteralTooShort
                                   : JsonParseError::kInvalidLiteral);
      }
      break;
    case 'f':
      if (out_sv.length() >= 5 && out_sv.substr(0, 5) == "false") {
        value_len = 5;
        out_value = false;
      } else {
        err = (out_sv.length() < 5 ? JsonParseError::kLiteralTooShort
                                   : JsonParseError::kInvalidLiteral);
      }
      break;
    case 'n':
      if (out_sv.length() >= 4 && out_sv.substr(0, 4) == "null") {
        value_len = 4;
        out_value = JsonNull{};
      } else {
        err = (out_sv.length() < 4 ? JsonParseError::kLiteralTooShort
                                   : JsonParseError::kInvalidLiteral);
      }
      break;
    default:
      if (out_sv.front() == '-' || std::isdigit(out_sv.front())) {
        err = internal::ParseNumber(out_sv, out_value, value_len);
      } else {
        err = JsonParseError::kInvalidStartChar;
      }
      break;
  }
  out_sv.remove_prefix(value_len);
  return err;
}

}  // namespace internal

class JsonObjectFieldIterator {
 public:
  explicit JsonObjectFieldIterator(std::string_view object_content)
      : remaining_(object_content) {
    if (internal::SkipWhitespace(remaining_); remaining_.empty()) {
      error_ = JsonParseError::kEmptyInput;
      return;
    }
    if (remaining_.front() != '{') {
      error_ = JsonParseError::kInvalidStartChar;
      return;
    }
    // Consume '{'
    remaining_.remove_prefix(1);
    LoadNextField();
  }

  void operator++() { LoadNextField(); }
  explicit operator bool() const {
    return !eof_ && error_ == JsonParseError::kSuccess;
  }

  const std::string_view& key() const { return key_; }
  const JsonValue& value() const { return value_; }
  JsonParseError error_code() const { return error_; }

 private:
  void LoadNextField() {
    if (remaining_.empty()) {
      error_ = JsonParseError::kUnterminatedObject;
      return;
    }
    if (remaining_.front() == '}') {
      eof_ = true;
      return;
    }
    error_ = internal::ParseJsonValue(remaining_, raw_key_, remaining_);
    if (error_ != JsonParseError::kSuccess) {
      return;
    }
    if (auto* sv_key = std::get_if<SimpleJsonString>(&raw_key_)) {
      key_ = sv_key->data;
    } else if (auto* s_key = std::get_if<ComplexJsonString>(&raw_key_)) {
      key_ = s_key->data;
    } else {
      error_ = JsonParseError::kKeyNotString;
      return;
    }

    internal::SkipWhitespace(remaining_);
    if (remaining_.empty() || remaining_.front() != ':') {
      error_ = JsonParseError::kMissingColon;
      return;
    }
    remaining_.remove_prefix(1);  // Consume ':'

    error_ = internal::ParseJsonValue(remaining_, value_, remaining_);
    if (error_ != JsonParseError::kSuccess) {
      return;
    }
    if (internal::SkipWhitespace(remaining_); remaining_.empty()) {
      // Didn't find '}' delimter to end the object.
      error_ = JsonParseError::kUnterminatedObject;
    } else if (remaining_.front() == ',') {
      remaining_.remove_prefix(1);
      if (internal::SkipWhitespace(remaining_);
          remaining_.empty() || remaining_.front() == '}') {
        error_ = JsonParseError::kMissingCommaOrTerminator;
      }
    } else if (remaining_.front() == '}') {
      // Don't do anything.
    } else {
      // No comma, but not terminated.
      error_ = JsonParseError::kMissingCommaOrTerminator;
    }
  }

  std::string_view remaining_;
  JsonValue raw_key_;
  std::string_view key_;
  JsonValue value_;
  bool eof_ = false;
  JsonParseError error_ = JsonParseError::kSuccess;
};

class JsonArrayElementIterator {
 public:
  explicit JsonArrayElementIterator(std::string_view arr) : remaining_(arr) {
    if (internal::SkipWhitespace(remaining_); remaining_.empty()) {
      error_ = JsonParseError::kEmptyInput;
      return;
    }
    if (remaining_.front() != '[') {
      error_ = JsonParseError::kInvalidStartChar;
      return;
    }
    // Consume '['
    remaining_.remove_prefix(1);
    LoadNextElement();
  }

  void operator++() { LoadNextElement(); }
  explicit operator bool() const {
    return !eof_ && error_ == JsonParseError::kSuccess;
  }

  const JsonValue& element() const { return element_; }
  JsonParseError error() const { return error_; }

 private:
  void LoadNextElement() {
    if (remaining_.empty()) {
      error_ = JsonParseError::kUnterminatedArray;
      return;
    }
    if (remaining_.front() == ']') {
      eof_ = true;
      return;
    }
    error_ = internal::ParseJsonValue(remaining_, element_, remaining_);
    if (error_ != JsonParseError::kSuccess) {
      return;
    }
    if (internal::SkipWhitespace(remaining_); remaining_.empty()) {
      error_ = JsonParseError::kUnterminatedArray;
    } else if (remaining_.front() == ',') {
      // Advance past the comma.
      remaining_.remove_prefix(1);
      if (internal::SkipWhitespace(remaining_);
          remaining_.empty() || remaining_.front() == ']') {
        error_ = JsonParseError::kMissingCommaOrTerminator;
      }
    } else if (remaining_.front() == ']') {
      // Don't do anything.
    } else {
      // No comma, but not empty: implies error for next element if any
      error_ = JsonParseError::kMissingCommaOrTerminator;
    }
  }

  std::string_view remaining_;
  JsonValue element_;
  bool eof_ = false;
  JsonParseError error_ = JsonParseError::kSuccess;
};

inline std::string_view GetStringValue(const JsonValue& value) {
  if (const auto* str = std::get_if<SimpleJsonString>(&value)) {
    return str->data;
  }
  if (const auto* str = std::get_if<ComplexJsonString>(&value)) {
    return str->data;
  }
  return {};
}

inline bool Exists(const JsonValue& value) {
  return !std::holds_alternative<JsonEmpty>(value);
}

inline std::optional<uint32_t> CoerceToUint32(const JsonValue& value) {
  if (const int64_t* i = std::get_if<int64_t>(&value)) {
    if (*i >= std::numeric_limits<uint32_t>::min() &&
        *i <= std::numeric_limits<uint32_t>::max()) {
      return static_cast<uint32_t>(*i);
    }
    return std::nullopt;
  }
  if (const double* d = std::get_if<double>(&value)) {
    if (*d >= std::numeric_limits<uint32_t>::min() &&
        *d <= std::numeric_limits<uint32_t>::max()) {
      return static_cast<uint32_t>(*d);
    }
    return std::nullopt;
  }
  return std::nullopt;
}

inline std::optional<int64_t> CoerceToTs(const JsonValue& value) {
  switch (value.index()) {
    case base::variant_index<JsonValue, double>():
      return static_cast<int64_t>(base::unchecked_get<double>(value) * 1000.0);
    case base::variant_index<JsonValue, int64_t>():
      return base::unchecked_get<int64_t>(value) * 1000;
    case base::variant_index<JsonValue, SimpleJsonString>():
      return CoerceToTs(
          std::string(base::unchecked_get<SimpleJsonString>(value).data));
    case base::variant_index<JsonValue, ComplexJsonString>():
      return CoerceToTs(base::unchecked_get<ComplexJsonString>(value).data);
    default:
      return std::nullopt;
  }
}

inline const JsonObject* GetObject(const JsonValue& value) {
  return std::get_if<JsonObject>(&value);
}

inline bool CoerceToBool(const JsonValue& value) {
  switch (value.index()) {
    case base::variant_index<JsonValue, bool>():
      return base::unchecked_get<bool>(value);
    case base::variant_index<JsonValue, JsonNull>():
      return false;
    case base::variant_index<JsonValue, int64_t>():
      return base::unchecked_get<int64_t>(value) != 0;
    case base::variant_index<JsonValue, double>(): {
      // According to JavaScript language zero or NaN is regarded as false
      const auto cf = std::fpclassify(base::unchecked_get<double>(value));
      return cf != FP_ZERO && cf != FP_NAN;
    }
    default:
      return false;
  }
}

}  // namespace perfetto::trace_processor::json

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_JSON_JSON_PARSER_H_
