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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_COLUMN_CHUNK_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_COLUMN_CHUNK_H_

#include <cstdint>
#include <variant>

#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

class ColumnView;

// kMaxBatchRows rows of one column: the buffer matching the column's type,
// plus which of its rows hold a value.
struct ColumnChunk {
  std::variant<FlexVector<uint32_t>,
               FlexVector<int32_t>,
               FlexVector<int64_t>,
               FlexVector<double>,
               FlexVector<StringPool::Id>,
               FlexVector<Variant>>
      values{FlexVector<uint32_t>()};
  BitVector validity;

  // Copies the first count logical rows into this chunk at offset, resolving
  // selections and preserving value bits and nulls.
  void CopyFrom(const ColumnView&, uint32_t count, uint32_t offset);

  // Views the populated chunk with the source's representation. Implicit Id
  // columns become stored Uint32 values. The chunk must outlive the view.
  ColumnView View(const ColumnView& source, bool nullable) const;

  // The chunk's values, made to hold kMaxBatchRows of T the first time they
  // are used.
  template <typename T>
  FlexVector<T>& Values() {
    // The default alternative is an empty vector, so holding the right type
    // is not enough to have room in it.
    if (!std::holds_alternative<FlexVector<T>>(values) ||
        base::unchecked_get<FlexVector<T>>(values).size() < kMaxBatchRows) {
      values = FlexVector<T>::CreateWithSize(kMaxBatchRows);
    }
    return base::unchecked_get<FlexVector<T>>(values);
  }

  // The chunk's values, which must already hold T.
  template <typename T>
  const FlexVector<T>& Values() const {
    return base::unchecked_get<FlexVector<T>>(values);
  }
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_COLUMN_CHUNK_H_
