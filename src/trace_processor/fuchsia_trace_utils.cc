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

int64_t TicksToNs(uint64_t ticks, uint64_t ticks_per_second) {
  return static_cast<int64_t>(ticks * uint64_t(1000000000) / ticks_per_second);
}

Variadic ArgValue::ToStorageVariadic(TraceStorage* storage) const {
  switch (type_) {
    case Type::kNull:
      return Variadic::String(storage->InternString("null"));
    case Type::kInt32:
      return Variadic::Integer(static_cast<int64_t>(int32_));
    case Type::kUint32:
      return Variadic::Integer(static_cast<int64_t>(uint32_));
    case Type::kInt64:
      return Variadic::Integer(int64_);
    case Type::kUint64:
      return Variadic::Integer(static_cast<int64_t>(uint64_));
    case Type::kDouble:
      return Variadic::Real(double_);
    case Type::kString:
      return Variadic::String(string_);
    case Type::kPointer:
      return Variadic::Integer(static_cast<int64_t>(pointer_));
    case Type::kKoid:
      return Variadic::Integer(static_cast<int64_t>(koid_));
    case Type::kUnknown:
      return Variadic::String(storage->InternString("unknown"));
  }
  PERFETTO_FATAL("Not reached");  // Make GCC happy.
}

}  // namespace fuchsia_trace_utils
}  // namespace trace_processor
}  // namespace perfetto
