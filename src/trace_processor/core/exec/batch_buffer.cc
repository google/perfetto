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

#include <cstdint>

namespace perfetto::trace_processor::core::exec {
base::Status BatchBuffer::Append(const RowBatch& in) {
  if (!in.size())
    return base::OkStatus();
  if (!batch_.size()) {
    batch_.CopyFrom(in);
    columns_.resize(in.column_count());
    for (uint32_t c = 0; c < in.column_count(); ++c) {
      if (in.owner(c))
        continue;
      auto& packed = columns_[c].packed;
      packed = columns_[c].chunks.Acquire();
      packed->CopyFrom(in.column(c), in.size(), 0);
      batch_.SetColumn(
          c, packed->View(in.column(c), in.column(c).validity() != nullptr),
          packed);
    }
    return base::OkStatus();
  }
  uint32_t before = batch_.size(), total = before + in.size();
  if (total > kMaxBatchRows || in.column_count() != batch_.column_count())
    return base::ErrStatus("batch buffer: incompatible batch size or schema");
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    if (!SameLogicalType(batch_.column(c), in.column(c)))
      return base::ErrStatus("batch buffer: column representation changed");
  }
  // TODO(lalitm): columns filled by one source share a selection, so this
  // builds the same indices once per column. Build them once per distinct
  // selection instead, reusing how RowBatch shares composed selections.
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    auto a = batch_.column(c);
    const auto& b = in.column(c);
    Column& column = columns_[c];
    if (a.kind() == b.kind() && a.data() == b.data() &&
        a.validity() == b.validity()) {
      auto& indices = column.indices;
      if (!indices) {
        indices = column.index_pool.Acquire();
        indices->resize(kMaxBatchRows);
        for (uint32_t i = 0; i < before; ++i)
          (*indices)[i] = a.selection().GetIndex(i);
      }
      for (uint32_t i = 0; i < in.size(); ++i)
        (*indices)[before + i] = b.selection().GetIndex(i);
      a.SetOwnedRows(indices, total);
      batch_.SetColumn(c, a, batch_.owner(c));
    } else {
      auto& packed = column.packed;
      if (!packed) {
        packed = column.chunks.Acquire();
        packed->CopyFrom(a, before, 0);
      }
      packed->CopyFrom(b, in.size(), before);
      batch_.SetColumn(c, packed->View(a, a.validity() || b.validity()),
                       packed);
    }
  }
  batch_.SetCardinality(total);
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::core::exec
