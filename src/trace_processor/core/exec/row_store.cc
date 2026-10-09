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

#include "src/trace_processor/core/exec/row_store.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

RowStore::RowStore() = default;
RowStore::~RowStore() = default;

base::Status RowStore::Append(const RowBatch& in) {
  if (!in.size()) {
    return base::OkStatus();
  }
  if (in.size() > std::numeric_limits<uint32_t>::max() - size_) {
    return base::ErrStatus("row store: too many rows");
  }
  if (size_) {
    const RowBatch& first = *batches_[0];
    if (first.column_count() != in.column_count()) {
      return base::ErrStatus("row store: column count changed");
    }
    for (uint32_t c = 0; c < in.column_count(); ++c) {
      if (!SameLogicalType(first.column(c), in.column(c))) {
        return base::ErrStatus("row store: column representation changed");
      }
    }
  } else {
    nullable_.assign(in.column_count(), false);
  }
  if (held_ == batches_.size()) {
    batches_.push_back(std::make_unique<RowBatch>());
  }
  batches_[held_++]->CopyFrom(in);
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    if (in.column(c).validity()) {
      nullable_[c] = true;
    }
  }
  size_ += in.size();
  batch_of_row_.resize(size_, static_cast<uint32_t>(ends_.size()));
  ends_.push_back(size_);
  return base::OkStatus();
}

void RowStore::Clear() {
  for (uint32_t i = 0; i < held_; ++i) {
    batches_[i]->Reset();
  }
  held_ = 0;
  ends_.clear();
  batch_of_row_.clear();
  nullable_.clear();
  size_ = 0;
}

uint32_t RowStore::Find(uint32_t row) const {
  PERFETTO_DCHECK(row < size_);
  return batch_of_row_[row];
}

uint32_t RowStore::View(RowBatch* out, uint32_t offset, uint32_t count) const {
  out->Reset();
  if (!count) {
    return 0;
  }
  uint32_t batch = Find(offset);
  uint32_t start = batch ? ends_[batch - 1] : 0;
  count = std::min(count, ends_[batch] - offset);
  out->CopyFrom(*batches_[batch]);
  out->mutable_selection().KeepRange(offset - start, count);
  return count;
}

uint32_t RowStore::View(RowBatch* out,
                        Span<const uint32_t> rows,
                        Context& context) {
  out->Reset();
  auto count =
      static_cast<uint32_t>(std::min<size_t>(rows.size(), kMaxBatchRows));
  if (!count) {
    return 0;
  }
  // Which batch each row is in, and where in it: found once for every
  // column. Rows taken out of order rarely come in runs from one batch, so
  // each column is then copied a row at a time, not a run at a time.
  std::array<uint32_t, kMaxBatchRows> sources;
  std::array<uint32_t, kMaxBatchRows> batch_rows;
  uint32_t batch = Find(rows[0]);
  uint32_t start = batch ? ends_[batch - 1] : 0;
  for (uint32_t r = 0; r < count; ++r) {
    if (rows[r] < start || rows[r] >= ends_[batch]) {
      batch = Find(rows[r]);
      start = batch ? ends_[batch - 1] : 0;
    }
    sources[r] = batch;
    batch_rows[r] = batches_[batch]->selection()[rows[r] - start];
  }
  const RowBatch& first = *batches_[0];
  views_.resize(held_);
  for (uint32_t c = 0; c < first.column_count(); ++c) {
    for (uint32_t b = 0; b < held_; ++b) {
      views_[b] = &batches_[b]->column(c);
    }
    ColumnBuffer buffer = context.TakeBuffer();
    ColumnChunk& chunk = buffer.chunk();
    chunk.Gather(views_, sources.data(), batch_rows.data(), count, &bases_);
    out->AddColumn(chunk.View(first.column(c), nullable_[c]),
                   std::move(buffer));
  }
  out->SetRowCount(count);
  return count;
}

}  // namespace perfetto::trace_processor::core::exec
