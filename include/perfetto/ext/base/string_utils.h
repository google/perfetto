/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BASE_STRING_UTILS_H_
#define INCLUDE_PERFETTO_EXT_BASE_STRING_UTILS_H_

#include <string>
#include <vector>

#include <inttypes.h>
#include <stdlib.h>

#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_view.h"

namespace perfetto {
namespace base {

inline char Lowercase(char c) {
  return ('A' <= c && c <= 'Z') ? static_cast<char>(c - ('A' - 'a')) : c;
}

inline char Uppercase(char c) {
  return ('a' <= c && c <= 'z') ? static_cast<char>(c + ('A' - 'a')) : c;
}

inline Optional<uint32_t> CStringToUInt32(const char* s) {
  char* endptr = nullptr;
  uint64_t value = strtoul(s, &endptr, 10);
  Optional<uint32_t> result(base::nullopt);
  if (*s != '\0' && *endptr == '\0')
    result = static_cast<uint32_t>(value);
  return result;
}

inline Optional<int32_t> CStringToInt32(const char* s) {
  char* endptr = nullptr;
  int64_t value = strtol(s, &endptr, 10);
  Optional<int32_t> result(base::nullopt);
  if (*s != '\0' && *endptr == '\0')
    result = static_cast<int32_t>(value);
  return result;
}

inline Optional<double> CStringToDouble(const char* s) {
  char* endptr = nullptr;
  double value = strtod(s, &endptr);
  Optional<double> result(base::nullopt);
  if (*s != '\0' && *endptr == '\0')
    result = value;
  return result;
}

inline Optional<uint32_t> StringToUInt32(const std::string& s) {
  return CStringToUInt32(s.c_str());
}

inline Optional<int32_t> StringToInt32(const std::string& s) {
  return CStringToInt32(s.c_str());
}

inline Optional<double> StringToDouble(const std::string& s) {
  return CStringToDouble(s.c_str());
}

bool StartsWith(const std::string& str, const std::string& prefix);
bool EndsWith(const std::string& str, const std::string& suffix);
bool Contains(const std::string& haystack, const std::string& needle);
size_t Find(const StringView& needle, const StringView& haystack);
bool CaseInsensitiveEqual(const std::string& first, const std::string& second);
std::string Join(const std::vector<std::string>& parts,
                 const std::string& delim);
std::vector<std::string> SplitString(const std::string& text,
                                     const std::string& delimiter);
std::string StripPrefix(const std::string& str, const std::string& prefix);
std::string StripSuffix(const std::string& str, const std::string& suffix);
std::string ToLower(const std::string& str);
std::string ToUpper(const std::string& str);
std::string StripChars(const std::string& str,
                       const std::string& chars,
                       char replacement);
std::string ToHex(const char* data, size_t size);
inline std::string ToHex(const std::string& s) {
  return ToHex(s.c_str(), s.size());
}

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_STRING_UTILS_H_
