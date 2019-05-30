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

#ifndef SRC_TRACE_PROCESSOR_SYSTRACE_UTILS_H_
#define SRC_TRACE_PROCESSOR_SYSTRACE_UTILS_H_

#include <string>

#include "perfetto/base/optional.h"
#include "perfetto/base/string_view.h"

namespace perfetto {
namespace trace_processor {
namespace systrace_utils {

struct SystraceTracePoint {
  SystraceTracePoint() {}

  SystraceTracePoint(char p, uint32_t tg, base::StringView n, double v)
      : phase(p), tgid(tg), name(std::move(n)), value(v) {}

  // Phase can be one of B, E or C.
  char phase = '\0';

  uint32_t tgid = 0;

  // For phase = 'B' and phase = 'C' only.
  base::StringView name;

  // For phase = 'C' only.
  double value = 0;
};

inline bool operator==(const SystraceTracePoint& x,
                       const SystraceTracePoint& y) {
  return std::tie(x.phase, x.tgid, x.name, x.value) ==
         std::tie(y.phase, y.tgid, y.name, y.value);
}

enum class SystraceParseResult { kFailure = 0, kUnsupported, kSuccess };

// We have to handle trace_marker events of a few different types:
// 1. some random text
// 2. B|1636|pokeUserActivity
// 3. E|1636
// 4. C|1636|wq:monitor|0
inline SystraceParseResult ParseSystraceTracePoint(base::StringView str,
                                                   SystraceTracePoint* out) {
  const char* s = str.data();
  size_t len = str.size();

  if (len < 2)
    return SystraceParseResult::kFailure;

  // If str matches '[BEC]\|[0-9]+[\|\n]' set tgid_length to the length of
  // the number. Otherwise return kFailure.
  if (s[1] != '|' && s[1] != '\n')
    return SystraceParseResult::kFailure;
  if (s[0] != 'B' && s[0] != 'E' && s[0] != 'C') {
    // TODO: support android async slices
    return s[0] == 'S' || s[0] == 'F' ? SystraceParseResult::kUnsupported
                                      : SystraceParseResult::kFailure;
  }
  size_t tgid_length = 0;
  for (size_t i = 2; i < len; i++) {
    if (s[i] == '|' || s[i] == '\n') {
      tgid_length = i - 2;
      break;
    }
    if (s[i] < '0' || s[i] > '9')
      return SystraceParseResult::kFailure;
  }

  if (tgid_length == 0) {
    out->tgid = 0;
  } else {
    std::string tgid_str(s + 2, tgid_length);
    out->tgid = static_cast<uint32_t>(std::stoi(tgid_str.c_str()));
  }

  out->phase = s[0];
  switch (s[0]) {
    case 'B': {
      size_t name_index = 2 + tgid_length + 1;
      out->name = base::StringView(
          s + name_index, len - name_index - (s[len - 1] == '\n' ? 1 : 0));
      return SystraceParseResult::kSuccess;
    }
    case 'E': {
      return SystraceParseResult::kSuccess;
    }
    case 'C': {
      size_t name_index = 2 + tgid_length + 1;
      base::Optional<size_t> name_length;
      for (size_t i = name_index; i < len; i++) {
        if (s[i] == '|') {
          name_length = i - name_index;
          break;
        }
      }
      if (!name_length.has_value())
        return SystraceParseResult::kFailure;
      out->name = base::StringView(s + name_index, name_length.value());

      size_t value_index = name_index + name_length.value() + 1;
      size_t value_len = len - value_index;
      if (value_len == 0)
        return SystraceParseResult::kFailure;
      std::string value_str(s + value_index, value_len);
      out->value = std::stod(value_str.c_str());
      return SystraceParseResult::kSuccess;
    }
    default:
      return SystraceParseResult::kFailure;
  }
}

}  // namespace systrace_utils
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SYSTRACE_UTILS_H_
