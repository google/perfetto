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

#ifndef SRC_TRACE_PROCESSOR_VARIADIC_H_
#define SRC_TRACE_PROCESSOR_VARIADIC_H_

#include "src/trace_processor/string_pool.h"

namespace perfetto {
namespace trace_processor {

// Variadic type representing value of different possible types.
struct Variadic {
  enum Type { kInt, kString, kReal };

  static Variadic Integer(int64_t int_value) {
    Variadic variadic;
    variadic.type = Type::kInt;
    variadic.int_value = int_value;
    return variadic;
  }

  static Variadic String(StringPool::Id string_id) {
    Variadic variadic;
    variadic.type = Type::kString;
    variadic.string_value = string_id;
    return variadic;
  }

  static Variadic Real(double real_value) {
    Variadic variadic;
    variadic.type = Type::kReal;
    variadic.real_value = real_value;
    return variadic;
  }

  Type type;
  union {
    int64_t int_value;
    StringPool::Id string_value;
    double real_value;
  };
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_VARIADIC_H_
