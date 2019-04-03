/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/json_trace_utils.h"

#include <json/value.h>
#include <limits>

#if !PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)
#error The JSON trace parser is supported only in the standalone build for now.
#endif

namespace perfetto {
namespace trace_processor {
namespace json_trace_utils {

// Json trace event timestamps are in us.
// https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/edit#heading=h.nso4gcezn7n1
base::Optional<int64_t> CoerceToNs(const Json::Value& value) {
  switch (static_cast<size_t>(value.type())) {
    case Json::realValue:
      return static_cast<int64_t>(value.asDouble() * 1000);
    case Json::uintValue:
    case Json::intValue:
      return value.asInt64() * 1000;
    case Json::stringValue: {
      std::string s = value.asString();
      char* end;
      int64_t n = strtoll(s.c_str(), &end, 10);
      if (end != s.data() + s.size())
        return base::nullopt;
      return n * 1000;
    }
    default:
      return base::nullopt;
  }
}

base::Optional<int64_t> CoerceToInt64(const Json::Value& value) {
  switch (static_cast<size_t>(value.type())) {
    case Json::realValue:
    case Json::uintValue:
    case Json::intValue:
      return value.asInt64();
    case Json::stringValue: {
      std::string s = value.asString();
      char* end;
      int64_t n = strtoll(s.c_str(), &end, 10);
      if (end != s.data() + s.size())
        return base::nullopt;
      return n;
    }
    default:
      return base::nullopt;
  }
}

base::Optional<uint32_t> CoerceToUint32(const Json::Value& value) {
  base::Optional<int64_t> result = CoerceToInt64(value);
  if (!result.has_value())
    return base::nullopt;
  int64_t n = result.value();
  if (n < 0 || n > std::numeric_limits<uint32_t>::max())
    return base::nullopt;
  return static_cast<uint32_t>(n);
}

}  // namespace json_trace_utils
}  // namespace trace_processor
}  // namespace perfetto
