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

#include "src/trace_processor/core/exec/tree_accumulate.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Resolves a chunk's column into contiguous memory.
//
// A range selection already is contiguous, so it is handed back as a pointer
// into the storage the column references and nothing is copied; anything else
// is gathered once, here. Reading through the selection a row at a time
// inside the loop that follows would neither vectorize nor prefetch, which is
// why the filters resolve their columns the same way before looping.
const int64_t* Flatten(const ColumnView& column,
                       uint32_t count,
                       std::vector<int64_t>* scratch) {
  const auto* data = static_cast<const int64_t*>(column.data());
  RowSelection selection = column.selection();
  if (selection.is_range()) {
    return data + selection.offset();
  }
  scratch->resize(count);
  const uint32_t* rows = selection.data();
  for (uint32_t i = 0; i < count; ++i) {
    (*scratch)[i] = data[rows[i]];
  }
  return scratch->data();
}

}  // namespace

TreeAccumulateUp::TreeAccumulateUp(Source& source,
                                   RowBatchPool* pool,
                                   uint32_t parent_column,
                                   uint32_t value_column)
    : Breaker(source),
      pool_(pool),
      parent_column_(parent_column),
      value_column_(value_column) {}

TreeAccumulateUp::~TreeAccumulateUp() = default;

void TreeAccumulateUp::Rewind() {
  parents_.clear();
  values_.clear();
  sizes_.clear();
  retained_.clear();
  totals_.clear();
  next_ = 0;
}

base::Status TreeAccumulateUp::Consume(RowBatch& chunk) {
  uint32_t count = chunk.size();
  std::vector<int64_t> parent_scratch;
  std::vector<int64_t> value_scratch;
  const int64_t* parent =
      Flatten(chunk.column(parent_column_), count, &parent_scratch);
  const int64_t* value =
      Flatten(chunk.column(value_column_), count, &value_scratch);
  parents_.insert(parents_.end(), parent, parent + count);
  values_.insert(values_.end(), value, value + count);
  sizes_.push_back(count);

  // The chunk is kept by copying its values into a batch this operator owns.
  // A view would be cheaper and would be wrong: the source is free to refill
  // its buffers on the next pull, and one that materialises rows does exactly
  // that. Owning them also lands every column dense from row zero, which is
  // what lets the total be added beside them on the way out.
  PooledRowBatch held = pool_->Acquire();
  for (uint32_t col = 0; col < chunk.column_count(); ++col) {
    held->AddOwnedColumn(chunk.column(col), count);
  }
  held->SetCardinality(count);
  retained_.push_back(std::move(held));
  return base::OkStatus();
}

base::Status TreeAccumulateUp::Finish() {
  // Every parent precedes its children, so one pass backwards carries each
  // node's subtree into its parent.
  auto rows = static_cast<uint32_t>(parents_.size());
  std::vector<int64_t> totals(values_.begin(), values_.end());
  for (uint32_t row = rows; row-- > 0;) {
    int64_t parent = parents_[row];
    if (parent >= 0) {
      totals[static_cast<uint32_t>(parent)] += totals[row];
    }
  }
  uint32_t offset = 0;
  totals_.reserve(sizes_.size());
  for (uint32_t size : sizes_) {
    totals_.emplace_back(totals.begin() + offset,
                         totals.begin() + offset + size);
    offset += size;
  }
  return base::OkStatus();
}

RowBatch* TreeAccumulateUp::Emit() {
  if (next_ == retained_.size()) {
    return nullptr;
  }
  size_t index = next_++;
  RowBatch& chunk = *retained_[index];
  // The column is added on the way out rather than on the way in, because
  // until every row had gone past there was nothing to put in it.
  chunk.AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, totals_[index].data()));
  return &chunk;
}

}  // namespace perfetto::trace_processor::core::exec
