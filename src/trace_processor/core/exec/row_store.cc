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
#include <vector>

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

base::Status RowStore::Append(const RowBatch& in, Context& context) {
  if (!has_shape_) {
    SetShape(in, context);
  }
  if (!in.size()) {
    return base::OkStatus();
  }
  if (in.size() > std::numeric_limits<uint32_t>::max() - size_) {
    return base::ErrStatus("row store: too many rows");
  }
  // The planner guarantees every batch of a step has the same columns.
  PERFETTO_DCHECK(shape_.column_count() == in.column_count());
#if PERFETTO_DCHECK_IS_ON()
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    PERFETTO_DCHECK(SameLogicalType(ColumnShape::Of(shape_.column(c)),
                                    ColumnShape::Of(in.column(c))));
  }
#endif
  size_ += in.size();
  if (!drops()) {
    Keep(in, context);
    return base::OkStatus();
  }
  trimmed_.Project(in, held_columns_);
  Keep(trimmed_, context);
  // Not to keep the batch's buffers until the next one.
  trimmed_.Reset();
  return base::OkStatus();
}

void RowStore::SetShape(const RowBatch& in, Context& context) {
  shape_.CopyFrom(in);
  has_shape_ = true;
  held_columns_.clear();
  stored_column_.assign(in.column_count(), kNotHeld);
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    const ColumnView& column = in.column(c);
    if (c < unread_.size() && unread_[c] &&
        column.kind() != ColumnView::Kind::kVariant) {
      // Read back as nulls, typed as a stored Id would be.
      StorageType type =
          column.kind() == ColumnView::Kind::kSequence || column.type().Is<Id>()
              ? StorageType{Uint32{}}
              : column.type();
      shape_.SetNullColumn(c, type, context);
      continue;
    }
    stored_column_[c] = static_cast<uint32_t>(held_columns_.size());
    held_columns_.push_back(c);
  }
}

void RowStore::Keep(const RowBatch& in, Context& context) {
  if (Shares(in)) {
    // Rows packed before come first.
    Flush();
    Hold(in);
    return;
  }
  RowBatch part;
  for (uint32_t done = 0; done < in.size();) {
    uint32_t take = std::min(in.size() - done, kMaxBatchRows - packing_.size());
    part.CopyFrom(in);
    part.mutable_selection().KeepRange(done, take);
    packing_.Append(part, context);
    done += take;
    if (packing_.size() == kMaxBatchRows) {
      Flush();
    }
  }
}

bool RowStore::Shares(const RowBatch& in) {
  if (uint64_t{in.size()} * 2 >= kMaxBatchRows) {
    return true;
  }
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    if (in.buffer(c)) {
      return false;
    }
  }
  return true;
}

void RowStore::Hold(const RowBatch& in) {
  if (!held_) {
    nullable_ = FlexVector<bool>::CreateFilled(in.column_count(), false);
    sources_.resize(in.column_count());
  }
  if (held_ == batches_.size()) {
    batches_.push_back(std::make_unique<RowBatch>());
  }
  RowBatch& held = *batches_[held_];
  held.CopyFrom(in);
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    const ColumnView& column = held.column(c);
    sources_[c].push_back(GatherSource::Of(column));
    if (column.validity()) {
      nullable_[c] = true;
    }
  }
  held_rows_ += in.size();
  batch_of_row_.push_back_multiple(held_++, in.size());
  ends_.push_back(held_rows_);
}

void RowStore::Flush() {
  if (!packing_.size()) {
    return;
  }
  RowBatch packed;
  packing_.Take(packed);
  Hold(packed);
}

void RowStore::Clear() {
  for (uint32_t i = 0; i < held_; ++i) {
    batches_[i]->Reset();
  }
  held_ = 0;
  held_rows_ = 0;
  ends_.clear();
  batch_of_row_.clear();
  for (FlexVector<GatherSource>& sources : sources_) {
    sources.clear();
  }
  nullable_.clear();
  packing_.Clear();
  shape_.Reset();
  has_shape_ = false;
  held_columns_.clear();
  stored_column_.clear();
  size_ = 0;
}

uint32_t RowStore::View(RowBatch* out, uint32_t offset, uint32_t count) {
  out->Reset();
  if (!count) {
    return 0;
  }
  PERFETTO_DCHECK(offset < size_);
  Flush();
  uint32_t batch = Find(offset);
  uint32_t start = batch ? ends_[batch - 1] : 0;
  count = std::min(count, ends_[batch] - offset);
  const RowBatch& held = *batches_[batch];
  if (!drops()) {
    out->CopyFrom(held);
  } else {
    out->Project(held, {});
    for (uint32_t c = 0; c < shape_.column_count(); ++c) {
      uint32_t stored = stored_column_[c];
      if (stored == kNotHeld) {
        out->AddColumn(shape_.column(c), shape_.buffer(c));
      } else {
        out->AddColumn(held.column(stored), held.buffer(stored));
      }
    }
  }
  out->mutable_selection().KeepRange(offset - start, count);
  return count;
}

uint32_t RowStore::Gather(RowBatch* out,
                          Span<const uint32_t> rows,
                          Context& context) {
  out->Reset();
  auto count =
      static_cast<uint32_t>(std::min<size_t>(rows.size(), kMaxBatchRows));
  if (!count) {
    return 0;
  }
  Flush();
  // Rows held one after another in one batch are already laid out as wanted:
  // view them where they are.
  uint32_t first = rows[0];
  PERFETTO_DCHECK(first < size_);
  uint32_t batch = Find(first);
  const RowBatch& held = *batches_[batch];
  uint32_t start = batch ? ends_[batch - 1] : 0;
  bool run = held.selection().prefix() && first + count <= ends_[batch];
  for (uint32_t r = 1; run && r < count; ++r) {
    run = rows[r] == first + r;
  }
  if (run) {
    for (uint32_t c = 0; c < shape_.column_count(); ++c) {
      uint32_t stored = stored_column_[c];
      if (stored == kNotHeld) {
        out->AddColumn(shape_.column(c), shape_.buffer(c));
        continue;
      }
      ColumnView view = held.column(stored);
      view.set_start(view.start() + (first - start));
      out->AddColumn(view, held.buffer(stored));
    }
    out->SetRowCount(count);
    return count;
  }
  // Which batch each row is in, and which row of it: found once for every
  // column. Rows taken out of order rarely come in runs from one batch, so
  // each column is then copied a row at a time, not a run at a time.
  std::array<uint32_t, kMaxBatchRows> batches;
  std::array<uint32_t, kMaxBatchRows> batch_rows;
  for (uint32_t r = 0; r < count; ++r) {
    PERFETTO_DCHECK(rows[r] < size_);
    batches[r] = Find(rows[r]);
    batch_rows[r] = RowInBatch(rows[r], batches[r]);
  }
  for (uint32_t c = 0; c < shape_.column_count(); ++c) {
    const ColumnView& like = shape_.column(c);
    uint32_t stored = stored_column_[c];
    if (stored == kNotHeld) {
      out->AddColumn(like, shape_.buffer(c));
      continue;
    }
    ColumnBuffer buffer = context.TakeBuffer(ValueWidth(like));
    ColumnChunk& chunk = buffer.chunk();
    chunk.Gather(like, sources_[stored].data(), batches.data(),
                 batch_rows.data(), count, nullable_[stored]);
    out->AddColumn(chunk.View(like, nullable_[stored]), std::move(buffer));
  }
  out->SetRowCount(count);
  return count;
}

}  // namespace perfetto::trace_processor::core::exec
