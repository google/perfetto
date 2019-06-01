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
#include <stdarg.h>
#include <stdint.h>
#include <string>

namespace perfetto {
namespace trace_processor {

struct Config {
};

// Represents a dynamically typed value returned by SQL.
struct SqlValue {
  // Represents the type of the value.
  enum Type {
    kNull = 0,
    kString,
    kLong,
    kDouble,
    kBytes,
  };

  double AsDouble() {
    assert(type == kDouble);
    return double_value;
  }

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
