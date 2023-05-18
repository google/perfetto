/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/db/storage_overlay.h"

#include "src/trace_processor/db/storage_variants.h"

namespace perfetto {
namespace trace_processor {
namespace column {

void StorageOverlay::Filter(FilterOp op, SqlValue value, RowMap& rm) const {
  if (op == FilterOp::kIsNotNull)
    return;

  if (op == FilterOp::kIsNull) {
    rm.Clear();
    return;
  }

  BitVector::Builder builder(storage_->size());
  // Slow path: we compare <64 elements and append to get us to a word
  // boundary.
  uint32_t front_elements = builder.BitsUntilWordBoundaryOrFull();
  storage_->CompareSlow(op, value, 0, front_elements, builder);
  uint32_t cur_index = front_elements;

  // Fast path: we compare as many groups of 64 elements as we can.
  // This should be very easy for the compiler to auto-vectorize.
  uint32_t fast_path_elements = builder.BitsInCompleteWordsUntilFull();
  storage_->CompareFast(op, value, cur_index, fast_path_elements, builder);
  cur_index += fast_path_elements;

  // Slow path: we compare <64 elements and append to fill the Builder.
  uint32_t back_elements = builder.BitsUntilFull();
  storage_->CompareSlow(op, value, cur_index, back_elements, builder);

  BitVector bv = std::move(builder).Build();
  rm.Intersect(RowMap(std::move(bv)));
}

void StorageOverlay::StableSort(uint32_t* rows, uint32_t rows_size) const {
  storage_->StableSort(rows, rows_size);
}

}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
