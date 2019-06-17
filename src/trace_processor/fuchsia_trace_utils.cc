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

#include "src/trace_processor/fuchsia_trace_utils.h"

namespace perfetto {
namespace trace_processor {
namespace fuchsia_trace_utils {

namespace {
constexpr uint32_t kInlineStringMarker = 0x8000;
constexpr uint32_t kInlineStringLengthMask = 0x7FFF;
}  // namespace

bool IsInlineString(uint32_t string_ref) {
  // Treat a string ref of 0 (the empty string) as inline. The empty string is
  // not a true entry in the string table.
  return (string_ref & kInlineStringMarker) || (string_ref == 0);
}

base::StringView ReadInlineString(const uint64_t** current_ptr,
                                  uint32_t string_ref) {
  // Note that this works correctly for the empty string, where string_ref is 0.
  size_t len = string_ref & kInlineStringLengthMask;
  size_t len_words = (len + 7) / 8;
  base::StringView s(reinterpret_cast<const char*>(*current_ptr), len);
  *current_ptr += len_words;
  return s;
}

bool IsInlineThread(uint32_t thread_ref) {
  return thread_ref == 0;
}

ThreadInfo ReadInlineThread(const uint64_t** current_ptr) {
  ThreadInfo ret;
  ret.pid = **current_ptr;
  (*current_ptr)++;
  ret.tid = **current_ptr;
  (*current_ptr)++;
  return ret;
}

int64_t ReadTimestamp(const uint64_t** current_ptr, uint64_t ticks_per_second) {
  uint64_t ticks = **current_ptr;
  (*current_ptr)++;
  return TicksToNs(ticks, ticks_per_second);
}

// Converts a tick count to nanoseconds. Returns -1 if the result would not
// fit in a nonnegative int64_t. Negative timestamps are not allowed by the
// Fuchsia trace format.
int64_t TicksToNs(uint64_t ticks, uint64_t ticks_per_second) {
  uint64_t ticks_hi = ticks >> 32;
  uint64_t ticks_lo = ticks & ((uint64_t(1) << 32) - 1);
  uint64_t ns_per_sec = 1000000000;
  // This multiplication may overflow.
  uint64_t result_hi = ticks_hi * ((ns_per_sec << 32) / ticks_per_second);
  if (ticks_hi != 0 &&
      result_hi / ticks_hi != ((ns_per_sec << 32) / ticks_per_second)) {
    return -1;
  }
  // This computation never overflows, because ticks_lo is less than 2^32, and
  // ns_per_sec = 10^9 < 2^32.
  uint64_t result_lo = ticks_lo * ns_per_sec / ticks_per_second;
  // Performing addition before the cast avoids undefined behavior.
  int64_t result = static_cast<int64_t>(result_hi + result_lo);
  // Check for addition overflow.
  if (result < 0) {
    return -1;
  }
  return result;
}

Variadic ArgValue::ToStorageVariadic(TraceStorage* storage) const {
  switch (type_) {
    case ArgType::kNull:
      return Variadic::String(storage->InternString("null"));
    case ArgType::kInt32:
      return Variadic::Integer(static_cast<int64_t>(int32_));
    case ArgType::kUint32:
      return Variadic::Integer(static_cast<int64_t>(uint32_));
    case ArgType::kInt64:
      return Variadic::Integer(int64_);
    case ArgType::kUint64:
      return Variadic::Integer(static_cast<int64_t>(uint64_));
    case ArgType::kDouble:
      return Variadic::Real(double_);
    case ArgType::kString:
      return Variadic::String(string_);
    case ArgType::kPointer:
      return Variadic::Integer(static_cast<int64_t>(pointer_));
    case ArgType::kKoid:
      return Variadic::Integer(static_cast<int64_t>(koid_));
    case ArgType::kUnknown:
      return Variadic::String(storage->InternString("unknown"));
  }
  PERFETTO_FATAL("Not reached");  // Make GCC happy.
}

}  // namespace fuchsia_trace_utils
}  // namespace trace_processor
}  // namespace perfetto
