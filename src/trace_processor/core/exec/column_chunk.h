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
#include "perfetto/base/logging.h"
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

// The bytes one value of a column of `type` takes in a chunk: what a buffer
// for the column is taken for (see Context::TakeBuffer).
constexpr uint32_t ValueWidth(StorageType type) {
  // Id, Uint32, Int32 and String, stored as its interned id, are 4 bytes.
  return type.Is<Int64>() || type.Is<Double>() ? sizeof(int64_t)
                                               : sizeof(uint32_t);
}

// The bytes one of `view`'s values takes in a chunk.
inline uint32_t ValueWidth(const ColumnView& view) {
  return view.kind() == ColumnView::Kind::kVariant ? sizeof(Variant)
                                                   : ValueWidth(view.type());
}

// Where one batch's values of a column are, as a ColumnView has them, flat
// enough to be copied into an array: row `row` of the batch is the value and
// the bit at `start + row`.
struct GatherSource {
  static GatherSource Of(const ColumnView& view) {
    return {view.data(), view.validity(), view.start()};
  }

  // Null for a sequence, whose value is where it would be stored.
  const void* data;
  // Null when every value is there.
  const BitVector* validity;
  uint32_t start;
};

// kMaxBatchRows rows of one column: the buffer matching the column's type,
// plus which of its rows hold a value.
struct ColumnChunk {
  // kMaxBatchRows values of the width the chunk's buffer was taken for (see
  // BufferPool): allocated with the buffer, and reused, never reallocated, by
  // every column of that width which takes it after.
  Slab<uint8_t> storage;
  BitVector validity;

  // Copies the rows `batch` keeps of its `column` into this chunk from
  // `offset`, preserving value bits and nulls.
  void CopyFrom(const RowBatch& batch, uint32_t column, uint32_t offset);

  // Copies, for each i below `count`, batch row rows[i] of
  // sources[batches[i]] into this chunk at i. The sources hold the type of
  // `like`, a view of any of them, which is dispatched on once. `nullable` is
  // whether any of the sources has nulls.
  void Gather(const ColumnView& like,
              const GatherSource* sources,
              const uint32_t* batches,
              const uint32_t* rows,
              uint32_t count,
              bool nullable);

  // Copies the first `count` of `data`, present where `present` says if not
  // null, into this chunk, and views them.
  template <typename T>
  ColumnView Fill(const T* data, const BitVector* present, uint32_t count) {
    T* copy = Values<T>();
    memcpy(copy, data, count * sizeof(T));
    const BitVector* kept = nullptr;
    if (present) {
      SizeValidity();
      validity.FillBits(0, count, false);
      validity.OrBitsFrom(0, *present, 0, count);
      kept = &validity;
    }
    using Type = typename TypeTagFor<T>::type;
    return ColumnView::Reference(StorageType{Type{}}, copy, kept);
  }

  // Views the populated chunk with the source's representation. Implicit Id
  // columns become stored Uint32 values. The chunk must outlive the view.
  ColumnView View(const ColumnView& source, bool nullable) const;

  // Gives the validity kMaxBatchRows bits, which it then keeps.
  void SizeValidity() {
    if (PERFETTO_UNLIKELY(validity.size() != kMaxBatchRows)) {
      GrowValidity();
    }
  }

  // The chunk's kMaxBatchRows values of T. The chunk's buffer must have been
  // taken for values at least as wide as T.
  template <typename T>
  T* Values() {
    static_assert(sizeof(T) <= kMaxValueSize && alignof(T) <= kMaxValueSize);
    PERFETTO_DCHECK(uint64_t{kMaxBatchRows} * sizeof(T) <= storage.size());
    return reinterpret_cast<T*>(storage.data());
  }
  template <typename T>
  const T* Values() const {
    PERFETTO_DCHECK(uint64_t{kMaxBatchRows} * sizeof(T) <= storage.size());
    return reinterpret_cast<const T*>(storage.data());
  }

  // The size of the largest type a chunk holds.
  static constexpr size_t kMaxValueSize = sizeof(Variant);

 private:
  PERFETTO_NO_INLINE void GrowValidity();
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_COLUMN_CHUNK_H_
