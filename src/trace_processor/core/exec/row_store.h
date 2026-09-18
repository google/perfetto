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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

// Owns a copy of rows taken from one or more RowBatches.
//
// Views retain their backing chunks. Indexed views gather into pooled storage
// which is reused only after every published view releases it. Use BatchStore
// when retaining input buffers is sufficient and materialization is
// unnecessary.
//
// Rows are held in fixed-size chunks rather than one growing array, so nothing
// already appended is ever copied again. kChunkRows is a power of two, so a
// row's chunk and its offset within it are a shift and a mask.
class RowStore {
 public:
  static constexpr uint32_t kChunkRows = kMaxBatchRows;

  RowStore();
  ~RowStore();

  // Appends every column of `batch`. The first call fixes the column count and
  // the type of each column; later calls must match it.
  base::Status Append(const RowBatch& batch);

  uint32_t size() const { return size_; }
  uint32_t column_count() const {
    return static_cast<uint32_t>(columns_.size());
  }
  StorageType type(uint32_t column) const { return columns_[column].type; }
  bool is_variant(uint32_t column) const { return columns_[column].variant; }

  // Points `batch` at up to `count` rows from `offset` and returns how many it
  // could serve. A run never spans two chunks, so a caller asking for more than
  // the rest of a chunk gets the rest of the chunk.
  uint32_t View(RowBatch* batch, uint32_t offset, uint32_t count) const;

  // Points `batch` at the rows `rows` picks out, in that order. They can come
  // from any chunk, which no single view can span, so the values are gathered
  // into pooled storage retained by the output batch.
  void View(RowBatch* batch, Span<const uint32_t> rows);

  // Drops all rows, keeping only chunks which have no published views.
  void Clear();

 private:
  struct Column {
    StorageType type{Uint32{}};
    bool variant = false;
    bool nullable = false;
    std::vector<std::shared_ptr<ColumnChunk>> chunks;
    // Where a gathered view puts the values it picks out.
    BufferPool<ColumnChunk> buffers;
  };

  base::Status ValidateColumn(const Column&, const ColumnView&) const;
  void AppendColumn(Column&, const ColumnView&, uint32_t count);
  ColumnView ViewOf(const Column&, const ColumnChunk&) const;
  ColumnChunk& ChunkAt(Column&, uint32_t index) const;

  std::vector<Column> columns_;
  bool initialized_ = false;
  uint32_t size_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
