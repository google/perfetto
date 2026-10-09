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
#include <cstring>
#include <vector>

#include "perfetto/base/compiler.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

class RowBatch;

// kMaxBatchRows rows of one column: the buffer matching the column's type,
// plus which of its rows hold a value.
struct ColumnChunk {
  // kMaxBatchRows values of whichever type the chunk holds: allocated once,
  // as large as the largest type, so a chunk reused for another type, as
  // buffers are by whichever column next takes one, never reallocates.
  Slab<uint8_t> storage;
  BitVector validity;

  // Copies the rows `batch` keeps of its `column` into this chunk from
  // `offset`, preserving value bits and nulls.
  void CopyFrom(const RowBatch& batch, uint32_t column, uint32_t offset);

  // Copies, for each i below `count`, batch row rows[i] of views[sources[i]]
  // into this chunk at i. The views hold one type. The type is dispatched on
  // once, however the rows interleave the views; `bases` is scratch.
  void Gather(const std::vector<const ColumnView*>& views,
              const uint32_t* sources,
              const uint32_t* rows,
              uint32_t count,
              std::vector<const void*>* bases);

  // Copies the first `count` of `data`, present where `present` says if not
  // null, into this chunk, and views them.
  template <typename T>
  ColumnView Fill(const T* data, const BitVector* present, uint32_t count) {
    T* copy = Values<T>();
    memcpy(copy, data, count * sizeof(T));
    const BitVector* kept = nullptr;
    if (present) {
      validity.resize(kMaxBatchRows);
      validity.FillBits(0, count, false);
      validity.SetBitsFrom(0, *present, 0, count);
      kept = &validity;
    }
    using Type = typename TypeTagFor<T>::type;
    return ColumnView::Reference(StorageType{Type{}}, copy, kept);
  }

  // Views the populated chunk with the source's representation. Implicit Id
  // columns become stored Uint32 values. The chunk must outlive the view.
  ColumnView View(const ColumnView& source, bool nullable) const;

  // The chunk's kMaxBatchRows values of T, its storage allocated the first
  // time any are used.
  template <typename T>
  T* Values() {
    static_assert(sizeof(T) <= kMaxValueSize && alignof(T) <= kMaxValueSize);
    if (PERFETTO_UNLIKELY(!storage.size())) {
      storage = Slab<uint8_t>::Alloc(uint64_t{kMaxBatchRows} * kMaxValueSize);
    }
    return reinterpret_cast<T*>(storage.data());
  }
  template <typename T>
  const T* Values() const {
    return reinterpret_cast<const T*>(storage.data());
  }

  // The size of the largest type a chunk holds.
  static constexpr size_t kMaxValueSize = sizeof(Variant);
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_COLUMN_CHUNK_H_
