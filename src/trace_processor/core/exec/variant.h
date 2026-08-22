/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_VARIANT_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_VARIANT_H_

#include <cstdint>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"

namespace perfetto::trace_processor::core::exec {

// One value of a column whose type is per row rather than per column.
struct Variant {
  enum class Type : uint8_t { kNull, kInt64, kDouble, kString };

  static Variant Null() {
    Variant v;
    v.type = Type::kNull;
    v.int64_ = 0;
    return v;
  }
  static Variant Int64(int64_t value) {
    Variant v;
    v.type = Type::kInt64;
    v.int64_ = value;
    return v;
  }
  static Variant Double(double value) {
    Variant v;
    v.type = Type::kDouble;
    v.double_ = value;
    return v;
  }
  static Variant String(StringPool::Id value) {
    Variant v;
    v.type = Type::kString;
    v.string_ = value;
    return v;
  }

  int64_t AsInt64() const {
    PERFETTO_DCHECK(type == Type::kInt64);
    return int64_;
  }
  double AsDouble() const {
    PERFETTO_DCHECK(type == Type::kDouble);
    return double_;
  }
  StringPool::Id AsString() const {
    PERFETTO_DCHECK(type == Type::kString);
    return string_;
  }

  Type type;

 private:
  union {
    int64_t int64_;
    double double_;
    StringPool::Id string_;
  };
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_VARIANT_H_
