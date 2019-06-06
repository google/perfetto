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

#ifndef SRC_TRACE_PROCESSOR_FUCHSIA_TRACE_UTILS_H_
#define SRC_TRACE_PROCESSOR_FUCHSIA_TRACE_UTILS_H_

#include <stddef.h>
#include <stdint.h>
#include <functional>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {
namespace fuchsia_trace_utils {

struct ThreadInfo {
  uint64_t pid;
  uint64_t tid;
};

template <class T>
T ReadField(uint64_t word, size_t begin, size_t end) {
  return static_cast<T>((word >> begin) &
                        ((uint64_t(1) << (end - begin + 1)) - 1));
}

bool IsInlineString(uint32_t);
base::StringView ReadInlineString(const uint64_t**, uint32_t);

bool IsInlineThread(uint32_t);
ThreadInfo ReadInlineThread(const uint64_t**);

int64_t ReadTimestamp(const uint64_t**, uint64_t);
int64_t TicksToNs(uint64_t ticks, uint64_t ticks_per_second);

class ArgValue {
 public:
  enum Type {
    kNull,
    kInt32,
    kUint32,
    kInt64,
    kUint64,
    kDouble,
    kString,
    kPointer,
    kKoid,
    kUnknown,
  };

  static ArgValue Null() {
    ArgValue v;
    v.type_ = Type::kNull;
    v.int32_ = 0;
    return v;
  }

  static ArgValue Int32(int32_t value) {
    ArgValue v;
    v.type_ = Type::kInt32;
    v.int32_ = value;
    return v;
  }

  static ArgValue Uint32(uint32_t value) {
    ArgValue v;
    v.type_ = Type::kUint32;
    v.uint32_ = value;
    return v;
  }

  static ArgValue Int64(int64_t value) {
    ArgValue v;
    v.type_ = Type::kInt64;
    v.int64_ = value;
    return v;
  }

  static ArgValue Uint64(uint64_t value) {
    ArgValue v;
    v.type_ = Type::kUint64;
    v.uint64_ = value;
    return v;
  }

  static ArgValue Double(double value) {
    ArgValue v;
    v.type_ = Type::kDouble;
    v.double_ = value;
    return v;
  }

  static ArgValue String(StringId value) {
    ArgValue v;
    v.type_ = Type::kString;
    v.string_ = value;
    return v;
  }

  static ArgValue Pointer(uint64_t value) {
    ArgValue v;
    v.type_ = Type::kPointer;
    v.pointer_ = value;
    return v;
  }

  static ArgValue Koid(uint64_t value) {
    ArgValue v;
    v.type_ = Type::kKoid;
    v.koid_ = value;
    return v;
  }

  static ArgValue Unknown() {
    ArgValue v;
    v.type_ = Type::kUnknown;
    v.int32_ = 0;
    return v;
  }

  Variadic ToStorageVariadic(TraceStorage*) const;

 private:
  Type type_;
  union {
    int32_t int32_;
    uint32_t uint32_;
    int64_t int64_;
    uint64_t uint64_;
    double double_;
    StringId string_;
    uint64_t pointer_;
    uint64_t koid_;
  };
};

}  // namespace fuchsia_trace_utils
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_FUCHSIA_TRACE_UTILS_H_
