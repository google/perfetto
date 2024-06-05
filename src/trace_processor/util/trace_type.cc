/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/util/trace_type.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto::trace_processor {
namespace {
// Fuchsia traces have a magic number as documented here:
// https://fuchsia.googlesource.com/fuchsia/+/HEAD/docs/development/tracing/trace-format/README.md#magic-number-record-trace-info-type-0
constexpr uint64_t kFuchsiaMagicNumber = 0x0016547846040010;
constexpr char kPerfMagic[] = "PERFILE2";

inline bool isspace(unsigned char c) {
  return ::isspace(c);
}

std::string RemoveWhitespace(std::string str) {
  str.erase(std::remove_if(str.begin(), str.end(), isspace), str.end());
  return str;
}

}  // namespace

const char* ToString(TraceType trace_type) {
  switch (trace_type) {
    case kJsonTraceType:
      return "JSON trace";
    case kProtoTraceType:
      return "proto trace";
    case kNinjaLogTraceType:
      return "ninja log";
    case kFuchsiaTraceType:
      return "fuchsia trace";
    case kSystraceTraceType:
      return "systrace trace";
    case kGzipTraceType:
      return "gzip trace";
    case kCtraceTraceType:
      return "ctrace trace";
    case kZipFile:
      return "ZIP file";
    case kPerfDataTraceType:
      return "perf data";
    case kUnknownTraceType:
      return "unknown trace";
  }
  PERFETTO_FATAL("For GCC");
}

TraceType GuessTraceType(const uint8_t* data, size_t size) {
  if (size == 0)
    return kUnknownTraceType;
  std::string start(reinterpret_cast<const char*>(data),
                    std::min<size_t>(size, kGuessTraceMaxLookahead));
  if (size >= 8) {
    uint64_t first_word;
    memcpy(&first_word, data, sizeof(first_word));
    if (first_word == kFuchsiaMagicNumber)
      return kFuchsiaTraceType;
  }
  if (base::StartsWith(start, kPerfMagic)) {
    return kPerfDataTraceType;
  }
  std::string start_minus_white_space = RemoveWhitespace(start);
  if (base::StartsWith(start_minus_white_space, "{\""))
    return kJsonTraceType;
  if (base::StartsWith(start_minus_white_space, "[{\""))
    return kJsonTraceType;

  // Systrace with header but no leading HTML.
  if (base::Contains(start, "# tracer"))
    return kSystraceTraceType;

  // Systrace with leading HTML.
  // Both: <!DOCTYPE html> and <!DOCTYPE HTML> have been observed.
  std::string lower_start = base::ToLower(start);
  if (base::StartsWith(lower_start, "<!doctype html>") ||
      base::StartsWith(lower_start, "<html>"))
    return kSystraceTraceType;

  // Traces obtained from atrace -z (compress).
  // They all have the string "TRACE:" followed by 78 9C which is a zlib header
  // for "deflate, default compression, window size=32K" (see b/208691037)
  if (base::Contains(start, "TRACE:\n\x78\x9c"))
    return kCtraceTraceType;

  // Traces obtained from atrace without -z (no compression).
  if (base::Contains(start, "TRACE:\n"))
    return kSystraceTraceType;

  // Ninja's build log (.ninja_log).
  if (base::StartsWith(start, "# ninja log"))
    return kNinjaLogTraceType;

  // Systrace with no header or leading HTML.
  if (base::StartsWith(start, " "))
    return kSystraceTraceType;

  // gzip'ed trace containing one of the other formats.
  if (base::StartsWith(start, "\x1f\x8b"))
    return kGzipTraceType;

  if (base::StartsWith(start, "\x0a"))
    return kProtoTraceType;

  if (base::StartsWith(start, "PK\x03\x04"))
    return kZipFile;

  return kUnknownTraceType;
}

}  // namespace perfetto::trace_processor
