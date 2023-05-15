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

#include "src/trace_processor/db/null_overlay.h"

#include "src/trace_processor/db/storage_variants.h"

namespace perfetto {
namespace trace_processor {
namespace column {

void NullOverlay::Filter(FilterOp op, SqlValue sql_val, RowMap& rm) const {
  if (op == FilterOp::kIsNull) {
    rm.Intersect(RowMap(null_bv_->Not()));
    return;
  }
  if (op == FilterOp::kIsNotNull) {
    rm.Intersect(RowMap(null_bv_->Copy()));
    return;
  }

  // Row map for filtered data, not the size of whole column.
  RowMap filtered_data_rm(0, null_bv_->CountSetBits());
  inner_->Filter(op, sql_val, filtered_data_rm);

  // Select only rows that were not filtered out from null BitVector and
  // intersect it with RowMap&.
  rm.Intersect(RowMap(null_bv_->Copy()).SelectRows(filtered_data_rm));
}

void NullOverlay::StableSort(uint32_t* rows, uint32_t rows_size) const {
  uint32_t count_set_bits = null_bv_->CountSetBits();

  std::vector<uint32_t> non_null_rows(count_set_bits);
  std::vector<uint32_t> storage_to_rows(count_set_bits);

  // Saving the map from `out` index to `storage` index gives us free `IsSet()`
  // function, which would be very expensive otherwise.
  for (auto it = null_bv_->IterateSetBits(); it; it.Next()) {
    storage_to_rows[it.ordinal()] = it.index();
  }

  uint32_t cur_non_null_id = 0;
  uint32_t cur_null_id = 0;

  // Sort elements into null and non null.
  for (uint32_t i = 0; i < rows_size; ++i) {
    uint32_t row_idx = rows[i];
    auto it = std::lower_bound(storage_to_rows.begin(), storage_to_rows.end(),
                               row_idx);

    // This condition holds if the row is null.
    if (it == storage_to_rows.end() || *it != row_idx) {
      // We can override the out because we already used this data.
      rows[cur_null_id++] = row_idx;
      continue;
    }

    uint32_t non_null_idx =
        static_cast<uint32_t>(std::distance(storage_to_rows.begin(), it));
    non_null_rows[cur_non_null_id++] = non_null_idx;
  }

  // Sort storage and translate them into `rows` indices.
  inner_->StableSort(non_null_rows.data(), count_set_bits);
  uint32_t set_rows_offset = null_bv_->size() - count_set_bits;
  for (uint32_t i = 0; i < count_set_bits; ++i) {
    rows[set_rows_offset + i] = storage_to_rows[non_null_rows[i]];
  }
}

}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
