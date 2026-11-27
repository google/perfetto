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
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_vector.h"
#include "src/trace_processor/core/exec/selection.h"

namespace perfetto::trace_processor::core::exec {

base::Status RowStore::Append(const RowBatch& in, OperatorState& owner) {
  if (!has_shape_) {
    SetShape(in, owner);
  }
  if (!in.size()) {
    return base::OkStatus();
  }
  if (in.size() > std::numeric_limits<uint32_t>::max() - size_) {
    return base::ErrStatus("row store: too many rows");
  }
  // The planner guarantees every batch of a step has the same columns.
  PERFETTO_DCHECK(shape_->column_count() == in.column_count());
#if PERFETTO_DCHECK_IS_ON()
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    if (stored_column_[c] == kNotHeld) {
      continue;
    }
    PERFETTO_DCHECK(SameLogicalType(ColumnShape::Of(shape_->column(c)),
                                    ColumnShape::Of(in.column(c))));
  }
#endif
  size_ += in.size();
  if (!drops()) {
    Keep(in, owner);
    return base::OkStatus();
  }
  if (!trimmed_) {
    trimmed_ = owner.context().TakeBatch(owner);
  }
  trimmed_->Project(in, held_columns_.span());
  Keep(*trimmed_, owner);
  // Not to keep the batch's buffers until the next one.
  trimmed_->Reset();
  return base::OkStatus();
}

void RowStore::SetShape(const RowBatch& in, OperatorState& owner) {
  if (!shape_) {
    shape_ = owner.context().TakeBatch(owner);
  }
  shape_->CopyFrom(in);
  has_shape_ = true;
  held_columns_.clear();
  stored_column_.assign(owner, in.column_count(), kNotHeld);
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    const ColumnView& column = in.column(c);
    if (c < unread_.size() && unread_[c] &&
        column.kind() != ColumnView::Kind::kVariant) {
      // Read back as nulls.
      shape_->SetNullColumn(c, column.type(), owner.context());
      continue;
    }
    stored_column_[c] = held_columns_.size();
    held_columns_.push_back(owner, c);
  }
}

void RowStore::Keep(const RowBatch& in, OperatorState& owner) {
  if (Shares(in)) {
    // Rows packed before come first.
    Flush(owner);
    Hold(in, owner);
    return;
  }
  packed_ = true;
  RowBatch part;
  for (uint32_t done = 0; done < in.size();) {
    uint32_t take = std::min(in.size() - done, kMaxBatchRows - packing_.size());
    part.CopyFrom(in);
    part.mutable_selection().KeepRange(done, take);
    packing_.Append(part, owner);
    done += take;
    if (packing_.size() == kMaxBatchRows) {
      Flush(owner);
    }
  }
}

bool RowStore::Shares(const RowBatch& in) {
  if (uint64_t{in.size()} * 2 >= kMaxBatchRows) {
    return true;
  }
  bool buffers = false;
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    // A sparse column cannot be packed, and borrows its storage.
    if (in.column(c).kind() == ColumnView::Kind::kSparse) {
      return true;
    }
    buffers |= static_cast<bool>(in.buffer(c));
  }
  return !buffers;
}

void RowStore::Hold(const RowBatch& in, OperatorState& owner) {
  if (!held_) {
    nullable_.assign(owner, in.column_count(), false);
    sequence_.assign(owner, in.column_count(), false);
    uint32_t had = sources_.size();
    sources_.resize(owner, in.column_count());
    for (uint32_t c = had; c < sources_.size(); ++c) {
      sources_[c] = RunVector<GatherSource>();
    }
  }
  if (held_ == batches_.size()) {
    batches_.push_back(owner, owner.context().TakeBatch(owner));
  }
  RowBatch& held = *batches_[held_];
  held.CopyFrom(in);
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    const ColumnView& column = held.column(c);
    if (column.kind() == ColumnView::Kind::kSparse) {
      // Read only in place (see Place()): never gathered.
      holds_sparse_ = true;
      sources_[c].push_back(owner, {nullptr, nullptr, 0});
      continue;
    }
    sources_[c].push_back(owner, GatherSource::Of(column));
    if (column.validity()) {
      nullable_[c] = true;
    }
    if (column.kind() == ColumnView::Kind::kSequence) {
      sequence_[c] = true;
    }
  }
  if (dense_ == held_rows_ && held_rows_ == (held_ << kPlaceRowBits) &&
      in.selection().prefix()) {
    dense_ += in.size();
  } else {
    PERFETTO_DCHECK(held_ < 1u << (32 - kPlaceRowBits));
    const Selection& selection = in.selection();
    for (uint32_t i = 0; i < in.size(); ++i) {
      places_.push_back(owner, (held_ << kPlaceRowBits) | selection[i]);
    }
  }
  held_rows_ += in.size();
  ++held_;
  ends_.push_back(owner, held_rows_);
}

void RowStore::Flush(OperatorState& owner) {
  if (!packing_.size()) {
    return;
  }
  RowBatch packed;
  packing_.Take(packed);
  Hold(packed, owner);
}

void RowStore::Clear() {
  for (uint32_t i = 0; i < held_; ++i) {
    batches_[i]->Reset();
  }
  held_ = 0;
  held_rows_ = 0;
  ends_.clear();
  dense_ = 0;
  places_.clear();
  for (RunVector<GatherSource>& sources : sources_) {
    sources.clear();
  }
  nullable_.clear();
  sequence_.clear();
  packing_.Clear();
  packed_ = false;
  holds_sparse_ = false;
  for (uint32_t i = 0; i < views_.size(); ++i) {
    views_[i]->Reset();
  }
  if (shape_) {
    shape_->Reset();
  }
  has_shape_ = false;
  held_columns_.clear();
  stored_column_.clear();
  size_ = 0;
}

uint32_t RowStore::View(RowBatch* out,
                        uint32_t offset,
                        uint32_t count,
                        OperatorState& owner) {
  out->Reset();
  if (!count) {
    return 0;
  }
  PERFETTO_DCHECK(offset < size_);
  Flush(owner);
  uint32_t batch = Find(offset);
  uint32_t start = batch ? ends_[batch - 1] : 0;
  count = std::min(count, ends_[batch] - offset);
  ViewHeld(*batches_[batch], out);
  out->mutable_selection().KeepRange(offset - start, count);
  return count;
}

void RowStore::ViewHeld(const RowBatch& held, RowBatch* out) const {
  if (!drops()) {
    out->CopyFrom(held);
    return;
  }
  out->Project(held, {});
  for (uint32_t c = 0; c < shape_->column_count(); ++c) {
    uint32_t stored = stored_column_[c];
    if (stored == kNotHeld) {
      out->AddColumn(shape_->column(c), shape_->buffer(c));
    } else {
      out->AddColumn(held.column(stored), held.buffer(stored));
    }
  }
}

bool RowStore::Place(base::Span<const uint32_t> rows,
                     RowsInPlace* out,
                     OperatorState& owner) {
  if (packed_ || packing_.size()) {
    return false;
  }
  out->batches.clear();
  for (uint32_t b = 0; b < held_; ++b) {
    if (!drops()) {
      out->batches.push_back(owner, batches_[b]);
      continue;
    }
    if (views_.size() == b) {
      views_.push_back(owner, owner.context().TakeBatch(owner));
    }
    ViewHeld(*batches_[b], views_[b]);
    out->batches.push_back(owner, views_[b]);
  }
  auto count = static_cast<uint32_t>(rows.size());
  out->batch.resize(owner, count);
  out->row.resize(owner, count);
  for (uint32_t r = 0; r < count; ++r) {
    PERFETTO_DCHECK(rows[r] < size_);
    uint32_t batch = Find(rows[r]);
    out->batch[r] = batch;
    out->row[r] = RowInBatch(rows[r]);
  }
  return true;
}

uint32_t RowStore::Gather(RowBatch* out,
                          base::Span<const uint32_t> rows,
                          const Selection* kept,
                          OperatorState& owner) {
  PERFETTO_DCHECK(!holds_sparse_);
  out->Reset();
  auto count =
      static_cast<uint32_t>(std::min<size_t>(rows.size(), kMaxBatchRows));
  if (!count) {
    return 0;
  }
  Flush(owner);
  if (!kept && ViewIfRun(out, rows.data(), count)) {
    return count;
  }
  // The positions in `rows` of the rows to gather.
  std::array<uint16_t, kMaxBatchRows> prefix_rows;
  uint32_t kept_count = kept ? kept->size() : count;
  const uint16_t* kept_rows;
  if (kept && !kept->prefix()) {
    kept_rows = kept->indices();
  } else {
    PERFETTO_VECTORIZE_LOOP_ONCE
    for (uint32_t i = 0; i < kept_count; ++i) {
      prefix_rows[i] = static_cast<uint16_t>(i);
    }
    kept_rows = prefix_rows.data();
  }
  // Look up each row's place once, rather than once per column. While every
  // row so far is in the dense run, a row is its own place and there's
  // nothing to look up.
  std::array<uint32_t, kMaxBatchRows> found;
  const uint32_t* places = rows.data();
  if (!places_.empty()) {
    for (uint32_t i = 0; i < kept_count; ++i) {
      found[kept_rows[i]] = PlaceOf(rows[kept_rows[i]]);
    }
    places = found.data();
  }
  for (uint32_t c = 0; c < shape_->column_count(); ++c) {
    const ColumnView& like = shape_->column(c);
    uint32_t stored = stored_column_[c];
    if (stored == kNotHeld) {
      out->AddColumn(like, shape_->buffer(c));
      continue;
    }
    ColumnBuffer buffer = owner.context().TakeBuffer(ValueWidth(like));
    ColumnChunk& chunk = buffer.chunk();
    chunk.Gather(like, sources_[stored].data(), places, kept_rows, kept_count,
                 count, nullable_[stored], sequence_[stored]);
    out->AddColumn(chunk.View(like, nullable_[stored]), std::move(buffer));
  }
  out->SetRowCount(count);
  return count;
}

bool RowStore::ViewIfRun(RowBatch* out, const uint32_t* rows, uint32_t count) {
  uint32_t first = rows[0];
  PERFETTO_DCHECK(first < size_);
  uint32_t batch = Find(first);
  if (!batches_[batch]->selection().prefix() || first + count > ends_[batch]) {
    return false;
  }
  for (uint32_t r = 1; r < count; ++r) {
    if (rows[r] != first + r) {
      return false;
    }
  }
  ViewRun(out, batch, first, count);
  return true;
}

uint32_t RowStore::GatherInPlace(RowBatch* out,
                                 base::Span<const uint32_t> rows,
                                 OperatorState& owner) {
  auto count =
      static_cast<uint32_t>(std::min<size_t>(rows.size(), kMaxBatchRows));
  if (!count) {
    return Gather(out, rows, owner);
  }
  Flush(owner);
  uint32_t batch = Find(rows[0]);
  bool run = true;
  for (uint32_t r = 1; r < count; ++r) {
    if (Find(rows[r]) != batch) {
      return Gather(out, rows, owner);
    }
    run &= rows[r] == rows[0] + r;
  }
  const RowBatch& held = *batches_[batch];
  out->Reset();
  if (run && held.selection().prefix()) {
    // Viewed as a run, which readers take in one go.
    return ViewRun(out, batch, rows[0], count);
  }
  for (uint32_t c = 0; c < shape_->column_count(); ++c) {
    uint32_t stored = stored_column_[c];
    if (stored == kNotHeld) {
      out->AddColumn(shape_->column(c), shape_->buffer(c));
      continue;
    }
    out->AddColumn(held.column(stored), held.buffer(stored));
  }
  std::array<uint32_t, kMaxBatchRows> batch_rows;
  for (uint32_t r = 0; r < count; ++r) {
    batch_rows[r] = RowInBatch(rows[r]);
  }
  out->SetRowCount(held.selection().rows());
  out->mutable_selection().Reorder(
      base::Span<const uint32_t>(batch_rows.data(), batch_rows.data() + count));
  return count;
}

uint32_t RowStore::ViewRun(RowBatch* out,
                           uint32_t batch,
                           uint32_t first,
                           uint32_t count) const {
  const RowBatch& held = *batches_[batch];
  uint32_t start = batch ? ends_[batch - 1] : 0;
  for (uint32_t c = 0; c < shape_->column_count(); ++c) {
    uint32_t stored = stored_column_[c];
    if (stored == kNotHeld) {
      out->AddColumn(shape_->column(c), shape_->buffer(c));
      continue;
    }
    ColumnView view = held.column(stored);
    view.set_start(view.start() + (first - start));
    out->AddColumn(view, held.buffer(stored));
  }
  out->SetRowCount(count);
  return count;
}

}  // namespace perfetto::trace_processor::core::exec
