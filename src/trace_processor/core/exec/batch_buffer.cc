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

#include "src/trace_processor/core/exec/batch_buffer.h"

#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>

namespace perfetto::trace_processor::core::exec {
namespace {
bool Compatible(const ColumnView& a, const ColumnView& b) {
  return a.kind() == b.kind() &&
         (a.kind() == ColumnView::Kind::kVariant || a.type() == b.type());
}
template <typename T>
void CopyValues(const ColumnView& view,
                uint32_t count,
                uint32_t offset,
                ColumnChunk& chunk) {
  auto* dest = chunk.Values<T>().data() + offset;
  for (uint32_t i = 0; i < count; ++i)
    dest[i] = view.Value<T>(i);
}
void Copy(const ColumnView& view,
          uint32_t count,
          uint32_t offset,
          ColumnChunk& chunk) {
  if (view.kind() == ColumnView::Kind::kVariant) {
    CopyValues<Variant>(view, count, offset, chunk);
  } else if (view.type().Is<Id>() || view.type().Is<Uint32>()) {
    CopyValues<uint32_t>(view, count, offset, chunk);
  } else if (view.type().Is<Int32>()) {
    CopyValues<int32_t>(view, count, offset, chunk);
  } else if (view.type().Is<Int64>()) {
    CopyValues<int64_t>(view, count, offset, chunk);
  } else if (view.type().Is<Double>()) {
    CopyValues<double>(view, count, offset, chunk);
  } else {
    CopyValues<StringPool::Id>(view, count, offset, chunk);
  }
  chunk.validity.resize(kMaxBatchRows);
  for (uint32_t i = 0; i < count; ++i) {
    if (!view.validity() ||
        view.validity()->is_set(view.selection().GetIndex(i)))
      chunk.validity.set(offset + i);
    else
      chunk.validity.clear(offset + i);
  }
}
ColumnView Packed(const ColumnView& from,
                  const ColumnChunk& chunk,
                  bool nullable) {
  if (from.kind() == ColumnView::Kind::kVariant)
    return ColumnView::Variants(chunk.Values<Variant>().data());
  auto type = from.type().Is<Id>() ? StorageType{Uint32{}} : from.type();
  const void* data;
  if (type.Is<Uint32>())
    data = chunk.Values<uint32_t>().data();
  else if (type.Is<Int32>())
    data = chunk.Values<int32_t>().data();
  else if (type.Is<Int64>())
    data = chunk.Values<int64_t>().data();
  else if (type.Is<Double>())
    data = chunk.Values<double>().data();
  else
    data = chunk.Values<StringPool::Id>().data();
  return ColumnView::Reference(type, data,
                               nullable ? &chunk.validity : nullptr);
}
}  // namespace

base::Status BatchStore::Append(const RowBatch& in) {
  if (!in.size())
    return base::OkStatus();
  if (in.size() > std::numeric_limits<uint32_t>::max() - size_)
    return base::ErrStatus("batch store: too many rows");
  if (size_) {
    if (columns_.size() != in.column_count())
      return base::ErrStatus("batch store: column count changed");
    for (uint32_t c = 0; c < in.column_count(); ++c) {
      const auto& a = columns_[c].batches.front().view;
      const auto& b = in.column(c);
      if (!Compatible(a, b) && !(a.type().Is<Uint32>() && b.type().Is<Id>()))
        return base::ErrStatus("batch store: column representation changed");
    }
  }
  selections_.Reset();
  columns_.resize(in.column_count());
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    auto& column = columns_[c];
    auto view = in.column(c);
    view.RetainSelection(in.size(), selections_);
    auto owner = in.owner(c);
    // Unknown borrowed storage must be materialized before retention.
    if (!owner) {
      auto packed = std::make_shared<ColumnChunk>();
      Copy(view, in.size(), 0, *packed);
      view = Packed(view, *packed, view.validity() != nullptr);
      owner = std::move(packed);
    }
    if (!column.batches.empty()) {
      const auto& first = column.batches.front().view;
      column.shared_backing &= Compatible(first, view) &&
                               first.data() == view.data() &&
                               first.validity() == view.validity();
    }
    column.nullable |= view.validity() != nullptr;
    if (c) {
      auto previous = columns_[c - 1].batches.back().view.selection();
      auto selection = view.selection();
      column.same_selection_as_previous &=
          selection.data() == previous.data() &&
          selection.offset() == previous.offset();
    }
    column.batches.push_back({std::move(view), std::move(owner)});
  }
  size_ += in.size();
  batch_of_row_.resize(size_, static_cast<uint32_t>(ends_.size()));
  ends_.push_back(size_);
  return base::OkStatus();
}

uint32_t BatchStore::Find(uint32_t row) const {
  PERFETTO_DCHECK(row < size_);
  return batch_of_row_[row];
}
uint32_t BatchStore::View(RowBatch* out,
                          uint32_t offset,
                          uint32_t count) const {
  if (!count) {
    out->Reset();
    return 0;
  }
  uint32_t index = Find(offset);
  uint32_t start = index ? ends_[index - 1] : 0;
  count = std::min(count, ends_[index] - offset);
  out->Reset();
  for (const auto& column : columns_)
    out->AddColumn(column.batches[index].view, column.batches[index].owner);
  out->SetCardinality(ends_[index] - start);
  out->Slice(RowSelection::Range(offset - start), count);
  return count;
}
uint32_t BatchStore::View(RowBatch* out, Span<const uint32_t> rows) {
  out->Reset();
  selections_.Reset();
  uint32_t count =
      static_cast<uint32_t>(std::min<size_t>(rows.size(), kMaxBatchRows));
  if (!count)
    return 0;
  struct Location {
    uint32_t batch;
    uint32_t row;
  };
  std::array<Location, kMaxBatchRows> locations;
  uint32_t index = Find(rows[0]);
  uint32_t start = index ? ends_[index - 1] : 0;
  uint32_t run = 0;
  while (run < count && rows[run] >= start && rows[run] < ends_[index])
    ++run;
  // Keep a substantial existing run without gathering. Only fragmented
  // reordering benefits from packing across backing buffers.
  if (run >= (count + 1) / 2) {
    std::array<uint32_t, kMaxBatchRows> selection;
    for (uint32_t r = 0; r < run; ++r)
      selection[r] = rows[r] - start;
    out->Reset();
    for (const auto& column : columns_)
      out->AddColumn(column.batches[index].view, column.batches[index].owner);
    out->SetCardinality(ends_[index] - start);
    out->Slice(RowSelection::Indices(Span<const uint32_t>(
                   selection.data(), selection.data() + run)),
               run);
    return run;
  }
  for (uint32_t r = 0; r < count; ++r) {
    if (rows[r] < start || rows[r] >= ends_[index]) {
      index = Find(rows[r]);
      start = index ? ends_[index - 1] : 0;
    }
    locations[r] = {index, rows[r] - start};
  }
  std::array<uint32_t, kMaxBatchRows> physical;
  bool previous_shared = false;
  for (uint32_t c = 0; c < columns_.size(); ++c) {
    auto& column = columns_[c];
    if (!c || !column.same_selection_as_previous) {
      for (uint32_t r = 0; r < count; ++r) {
        const auto& loc = locations[r];
        physical[r] =
            column.batches[loc.batch].view.selection().GetIndex(loc.row);
      }
    }
    const auto& first = column.batches[locations[0].batch];
    auto view = first.view;
    bool shared = column.shared_backing;
    if (!shared) {
      shared = true;
      for (uint32_t r = 1; r < count; ++r) {
        const auto& other = column.batches[locations[r].batch].view;
        if (view.data() != other.data() ||
            view.validity() != other.validity() || !Compatible(view, other)) {
          shared = false;
          break;
        }
      }
    }
    if (shared) {
      if (previous_shared && column.same_selection_as_previous) {
        view.AdoptSelection(out->column(c - 1));
      } else {
        auto selection = selections_.TakeBlock();
        std::copy_n(physical.data(), count, selection->data());
        view.SetOwnedRows(std::move(selection), count);
      }
      out->AddColumn(view, first.owner);
      previous_shared = true;
      continue;
    }
    previous_shared = false;
    auto packed = column.buffers.Acquire();
    auto gather = [&](auto value) {
      using T = decltype(value);
      T* dest = packed->Values<T>().data();
      for (uint32_t r = 0; r < count; ++r) {
        const auto& loc = locations[r];
        auto* data =
            static_cast<const T*>(column.batches[loc.batch].view.data());
        if constexpr (std::is_same_v<T, uint32_t>)
          dest[r] = data ? data[physical[r]] : physical[r];
        else
          dest[r] = data[physical[r]];
      }
    };
    if (view.kind() == ColumnView::Kind::kVariant)
      gather(Variant{});
    else if (view.type().Is<Id>() || view.type().Is<Uint32>())
      gather(uint32_t{});
    else if (view.type().Is<Int32>())
      gather(int32_t{});
    else if (view.type().Is<Int64>())
      gather(int64_t{});
    else if (view.type().Is<Double>())
      gather(double{});
    else
      gather(StringPool::Id{});
    if (column.nullable) {
      packed->validity.clear();
      for (uint32_t begin = 0; begin < count; begin += 64) {
        uint64_t word = 0;
        uint32_t end = std::min(begin + 64, count);
        for (uint32_t r = begin; r < end; ++r) {
          const auto& loc = locations[r];
          const auto& source = column.batches[loc.batch].view;
          bool valid =
              !source.validity() || source.validity()->is_set(physical[r]);
          word |= static_cast<uint64_t>(valid) << (r - begin);
        }
        packed->validity.AppendWord(word);
      }
      packed->validity.resize(count);
    }
    out->AddColumn(Packed(view, *packed, column.nullable), packed);
  }
  out->SetCardinality(count);
  return count;
}
}  // namespace perfetto::trace_processor::core::exec
