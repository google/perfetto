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

#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/run_vector.h"

namespace perfetto::trace_processor::core::exec {

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
//
// Plain data, held for one run of the state `owner` which contains it (see
// RowStore).
class SortedRows {
 public:
  // Stores the rows of `in` and their values for `keys`. Pass the same
  // `keys` and `pool` for every batch. A key can be a string column only if
  // `pool`, holding its strings, is given. Fails if a key is neither, or if
  // its type changes. Rows packed are copied into buffers from the Context
  // (see RowStore).
  base::Status Append(const RowBatch& in,
                      base::Span<const SortKey> keys,
                      const StringPool* pool,
                      OperatorState& owner);
  // As Append(), with the rows' keys at `laid_out` as LayOutKeys() lays them
  // out, which saves doing so again.
  base::Status AppendLaidOut(const RowBatch& in,
                             base::Span<const SortKey> keys,
                             const StringPool* pool,
                             OperatorState& owner,
                             const uint8_t* laid_out);
  uint32_t size() const { return rows_.size(); }

  // Checks the keys of `in` as Append() does. The first call fixes their
  // types and layout.
  base::Status CheckKeys(const RowBatch& in,
                         base::Span<const SortKey> keys,
                         const StringPool* pool,
                         OperatorState& owner);
  // Lays out at `rows` the keys of each row `in` keeps, except string keys,
  // which are laid out only by Sort(). The keys must have been checked.
  void LayOutKeys(const RowBatch& in,
                  base::Span<const SortKey> keys,
                  uint8_t* rows) const;

  // How keys are laid out once checked: the number of bytes a row's keys take,
  // and the offset of key `key` among them.
  uint32_t stride() const { return stride_; }
  const RowLayout::Slot& slot(uint32_t key) const { return slots_[key]; }
  bool is_string(uint32_t key) const { return types_[key].string; }
  // The keys of the held row `row`, laid out.
  const uint8_t* keys(uint32_t row) const {
    return keys_.data() + uint64_t{row} * stride_;
  }
  // The string of the string key `key` at the held row `row`, if not null.
  std::optional<StringPool::Id> string(uint32_t key, uint32_t row) const;

  // Resets the order to input order, then stably sorts it by the keys. Nulls
  // sort as in SQLite: first when ascending, last when descending. Strings
  // sort by their contents.
  void Sort(OperatorState& owner);
  // The row numbers in output order.
  RunVector<uint32_t>& order() { return order_; }

  // After Sort(), points every row, in output order, where it is held, so
  // the rows are read without being copied, unless they cannot be (see
  // RowStore::Place).
  void Place(OperatorState& owner);
  // What Place() pointed at, or null if it did not.
  const RowsInPlace* placed() const { return placed_ ? &in_place_ : nullptr; }

  // Copies up to kMaxBatchRows rows into `out`, starting at position `at` in
  // the order, up to its end. Returns the number of rows copied.
  uint32_t Gather(RowBatch* out, uint32_t at, OperatorState& owner);
  // As Gather(), but rows held in one batch are viewed where they are held
  // (see RowStore::GatherInPlace).
  uint32_t GatherInPlace(RowBatch* out, uint32_t at, OperatorState& owner);
  void Clear();

  // Which columns are not held (see RowStore::SetUnread).
  void SetUnread(base::Span<const bool> unread) { rows_.SetUnread(unread); }

 private:
  struct KeyType {
    RowLayout::Type layout;
    // Laid out as its rank among the strings of the rows, once all are in.
    bool string;
  };
  // For a string key: each row's string, and whether it has one.
  struct Strings {
    RunVector<StringPool::Id> ids;
    RunBits present;
  };

  static std::optional<KeyType> GetKeyType(const ColumnView& column,
                                           bool strings);

  // Append() and AppendLaidOut(): `laid_out` is null for Append().
  base::Status Store(const RowBatch& in,
                     base::Span<const SortKey> keys,
                     const StringPool* pool,
                     OperatorState& owner,
                     const uint8_t* laid_out);

  RunVector<KeyType> types_;
  // One entry per key, empty except for string keys. Kept across Clear() for
  // reuse.
  RunVector<Strings> strings_;
  // Every string of the rows, and its rank once Sort() ranks them.
  RunMap ranks_;
  RunVector<StringPool::Id> ranked_;
  const StringPool* pool_ = nullptr;
  RunVector<RowLayout::Column> columns_;
  RunVector<RowLayout::Slot> slots_;
  uint32_t stride_ = 0;
  RunVector<uint8_t> keys_;
  RowStore rows_;
  RunVector<uint32_t> order_;
  RowsInPlace in_place_;
  bool placed_ = false;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_SORTED_ROWS_H_
