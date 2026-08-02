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

#ifndef SRC_TRACE_PROCESSOR_CORE_TREE_TREE_H_
#define SRC_TRACE_PROCESSOR_CORE_TREE_TREE_H_

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core {

// Simple, opinionated columnar storage for tree-structured data.
//
// Each column stores dense data as raw bytes (Slab<uint8_t>) with a dense null
// bitvector. Node ids are implicit dense row indices. The parent array contains
// row indices, with kNullParent used for roots.
//
// This is intentionally minimal: no sort state, no sparse nulls, and no shared
// ownership. Tree operators consume it and can reuse its allocations.
struct Tree {
  static constexpr uint32_t kNullParent = std::numeric_limits<uint32_t>::max();

  struct Column {
    using Type = TypeSet<Int64, Double, String>;

    template <typename T>
    static Column Create(uint32_t rows, bool nullable = false) {
      using TypeTag = typename TypeTagFor<T>::type;
      Column column;
      column.type = Type(TypeTag{});
      column.data = Slab<uint8_t>::Alloc(uint64_t(rows) * sizeof(T));
      if (nullable) {
        column.null_bv = BitVector::CreateWithSize(rows, true);
      }
      return column;
    }

    template <typename T>
    T* unchecked_data() {
      using TypeTag = typename TypeTagFor<T>::type;
      PERFETTO_DCHECK(type.Is<TypeTag>());
      return reinterpret_cast<T*>(data.data());
    }

    template <typename T>
    const T* unchecked_data() const {
      using TypeTag = typename TypeTagFor<T>::type;
      PERFETTO_DCHECK(type.Is<TypeTag>());
      return reinterpret_cast<const T*>(data.data());
    }

    template <typename T>
    Span<T> unchecked_span() {
      return Span<T>(unchecked_data<T>(),
                     unchecked_data<T>() + data.size() / sizeof(T));
    }

    template <typename T>
    Span<const T> unchecked_span() const {
      return Span<const T>(unchecked_data<T>(),
                           unchecked_data<T>() + data.size() / sizeof(T));
    }

    Type type = Type(Int64{});
    Slab<uint8_t> data;
    BitVector null_bv;  // non-empty for nullable columns
  };

  uint32_t row_count = 0;
  Slab<uint32_t> parent;  // normalized: row indices, kNullParent for roots
  std::vector<std::string> names;
  std::vector<Column> columns;
};

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_TREE_TREE_H_
