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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_SORTED_ROWS_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_SORTED_ROWS_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

class Context;

// A column rows are sorted by.
struct SortKey {
  uint32_t column = 0;
  bool descending = false;
};

// Rows held to be handed back in another order, for the breakers which
// reorder: each key's values are laid out as they arrive, the order is found
// on row numbers, by sorting on the keys and by whatever else its holder
// does to it, and the rows are copied out in it once.
class SortedRows {
 public:
  // Holds `in`'s rows and their values of `keys`, which are the same for
  // every batch. Fails if a key isn't a column of numbers of one type.
  base::Status Append(const RowBatch& in, const std::vector<SortKey>& keys);
  uint32_t size() const { return rows_.size(); }

  // Puts the order in the input's, then sorts it, stably, by the keys. Nulls
  // sort as in SQLite: first ascending, last descending.
  void Sort();
  // The rows held, by number, in the order they are handed back.
  FlexVector<uint32_t>& order() { return order_; }

  // Copies up to kMaxBatchRows rows, from the `at`th of the order on, into
  // buffers from `context`. Returns how many.
  uint32_t View(RowBatch* out, uint32_t at, Context& context);
  void Clear();

 private:
  std::vector<std::optional<RowLayout::Type>> types_;
  RowLayout layout_;
  FlexVector<uint8_t> keys_;
  RowStore rows_;
  FlexVector<uint32_t> order_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_SORTED_ROWS_H_
