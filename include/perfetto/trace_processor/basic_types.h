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

#ifndef INCLUDE_PERFETTO_TRACE_PROCESSOR_BASIC_TYPES_H_
#define INCLUDE_PERFETTO_TRACE_PROCESSOR_BASIC_TYPES_H_

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <functional>
#include <string>

#include "perfetto/base/export.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

// Struct for configuring a TraceProcessor instance (see trace_processor.h).
struct PERFETTO_EXPORT Config {
  // When set to true, this option forces trace processor to perform a full
  // sort ignoring any internal heureustics to skip sorting parts of the data.
  bool force_full_sort = false;

  // When set to a non-zero value, this overrides the default block size used
  // by the StringPool. For defaults, see kDefaultBlockSize in string_pool.h.
  size_t string_pool_block_size_bytes = 0;
};

// Represents a dynamically typed value returned by SQL.
struct PERFETTO_EXPORT SqlValue {
  // Represents the type of the value.
  enum Type {
    kNull = 0,
    kLong,
    kDouble,
    kString,
    kBytes,
  };

  SqlValue() = default;

  static SqlValue Long(int64_t v) {
    SqlValue value;
    value.long_value = v;
    value.type = Type::kLong;
    return value;
  }

  static SqlValue Double(double v) {
    SqlValue value;
    value.double_value = v;
    value.type = Type::kDouble;
    return value;
  }

  static SqlValue String(const char* v) {
    SqlValue value;
    value.string_value = v;
    value.type = Type::kString;
    return value;
  }

  double AsDouble() {
    assert(type == kDouble);
    return double_value;
  }

  int64_t Compare(const SqlValue& value) const {
    // TODO(lalitm): this is almost the same as what SQLite does with the
    // exception of comparisions between long and double - we choose (for
    // performance reasons) to omit comparisions between them.
    if (type != value.type)
      return type - value.type;

    switch (type) {
      case Type::kNull:
        return 0;
      case Type::kLong:
        return long_value - value.long_value;
      case Type::kDouble: {
        return double_value < value.double_value
                   ? -1
                   : (double_value > value.double_value ? 1 : 0);
      }
      case Type::kString:
        return strcmp(string_value, value.string_value);
      case Type::kBytes: {
        size_t bytes = std::min(bytes_count, value.bytes_count);
        int ret = memcmp(bytes_value, value.bytes_value, bytes);
        if (ret != 0)
          return ret;
        return static_cast<int64_t>(bytes_count - value.bytes_count);
      }
    }
    PERFETTO_FATAL("For GCC");
  }
  bool operator==(const SqlValue& value) const { return Compare(value) == 0; }
  bool operator<(const SqlValue& value) const { return Compare(value) < 0; }
  bool operator!=(const SqlValue& value) const { return !(*this == value); }
  bool operator>=(const SqlValue& value) const { return !(*this < value); }
  bool operator<=(const SqlValue& value) const { return !(value < *this); }
  bool operator>(const SqlValue& value) const { return value < *this; }

  bool is_null() const { return type == Type::kNull; }

  // Up to 1 of these fields can be accessed depending on |type|.
  union {
    // This string will be owned by the iterator that returned it and is valid
    // as long until the subsequent call to Next().
    const char* string_value;
    int64_t long_value;
    double double_value;
    const void* bytes_value;
  };
  // The size of bytes_value. Only valid when |type == kBytes|.
  size_t bytes_count = 0;
  Type type = kNull;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACE_PROCESSOR_BASIC_TYPES_H_
