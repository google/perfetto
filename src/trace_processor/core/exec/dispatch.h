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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_DISPATCH_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_DISPATCH_H_

#include <cstdint>
#include <type_traits>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/common/storage_types.h"

namespace perfetto::trace_processor::core::exec {

// Physical value type exposed by a vector. An ID has no backing values; its
// value is the selected physical row.
template <typename T>
using ValueOf =
    std::conditional_t<std::is_same_v<T, Id>, uint32_t, typename T::cpp_type>;

// Calls `fn` with the concrete type held by `type`. Dispatch once while
// constructing an operator or chunk reader; never dispatch inside a row loop.
template <typename Fn>
PERFETTO_ALWAYS_INLINE auto Dispatch(StorageType type, Fn&& fn) {
  switch (type.index()) {
    case StorageType::GetTypeIndex<Id>():
      return fn(Id{});
    case StorageType::GetTypeIndex<Uint32>():
      return fn(Uint32{});
    case StorageType::GetTypeIndex<Int32>():
      return fn(Int32{});
    case StorageType::GetTypeIndex<Int64>():
      return fn(Int64{});
    case StorageType::GetTypeIndex<Double>():
      return fn(Double{});
    case StorageType::GetTypeIndex<String>():
      return fn(String{});
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

// Number of bytes in one physical value. Narrow values are not widened.
inline uint32_t StoredWidth(StorageType type) {
  return Dispatch(type, [](auto t) -> uint32_t {
    return sizeof(ValueOf<decltype(t)>) == 8 ? 8u : 4u;
  });
}

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_DISPATCH_H_
