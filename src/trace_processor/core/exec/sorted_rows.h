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
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/common/string_ranks.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

class Context;

// A column to sort by, and its direction.
struct SortKey {
  uint32_t column = 0;
  bool descending = false;
};

// Stores rows so a breaker can output them in a different order.
//
// Key values are stored as rows arrive. The output order is a list of row
// numbers. Sort() sorts it by the keys, and the owner can change it further.
// Rows are copied out once, in that order.
class SortedRows {
 public:
  // Stores the rows of `in` and their values for `keys`. Pass the same
  // `keys` and `pool` for every batch. A key can be a string column only if
  // `pool`, holding its strings, is given. Fails if a key is neither, or if
  // its type changes.
  base::Status Append(const RowBatch& in,
                      const std::vector<SortKey>& keys,
                      const StringPool* pool);
  uint32_t size() const { return rows_.size(); }

  // Resets the order to input order, then stably sorts it by the keys. Nulls
  // sort as in SQLite: first when ascending, last when descending. Strings
  // sort by their contents.
  void Sort();
  // The row numbers in output order.
  FlexVector<uint32_t>& order() { return order_; }

  // Copies up to kMaxBatchRows rows into `out`, starting at position `at` in
  // the order. Returns the number of rows copied.
  uint32_t Gather(RowBatch* out, uint32_t at, Context& context);
  void Clear();

 private:
  struct KeyType {
    RowLayout::Type layout;
    // Laid out as its rank among the strings of the rows, once all are in.
    bool string;
  };
  // For a string key: each row's string, and whether it has one.
  struct Strings {
    FlexVector<StringPool::Id> ids;
    BitVector present;
  };

  static std::optional<KeyType> GetKeyType(const ColumnView& column,
                                           bool strings);

  std::vector<KeyType> types_;
  // By key; empty but for string keys.
  std::vector<Strings> strings_;
  StringRanks ranks_;
  const StringPool* pool_ = nullptr;
  RowLayout layout_;
  FlexVector<uint8_t> keys_;
  RowStore rows_;
  FlexVector<uint32_t> order_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_SORTED_ROWS_H_
