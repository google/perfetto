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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_GROUP_TABLE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_GROUP_TABLE_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

// The kind of values in a key column. It must be the same in every batch.
// Integers of any width are one kind, separate from doubles and strings.
enum class KeyKind : uint8_t { kInteger, kDouble, kString };

// Reads `count` selected rows of key column `column` into `words`. Integers
// are stored as int64, doubles as their canonical bits, strings as their
// interned id and nulls as 0. Sets bit `bit` of `nulls[i]` for each null row. A
// null string is not a null row: it is stored as the null id. Returns nullopt
// if the column cannot be a key.
std::optional<KeyKind> LoadKeyWords(const ColumnView& column,
                                    const Selection& selection,
                                    uint32_t count,
                                    uint32_t bit,
                                    int64_t* words,
                                    uint64_t* nulls);

// Hash table that assigns a number to each distinct key, in the order keys
// are first seen. As in DuckDB's aggregate hash table, each slot stores the
// top bits of the group's hash. Keys are only compared when those match.
//
// Integers of any width compare equal if their values are equal. Doubles are
// compared by their bits, with 0.0 and -0.0 equal, and strings by their
// interned id. Nulls are equal to each other and to nothing else. A column
// must hold the same kind of key in every batch.
class GroupTable {
 public:
  // Writes the group number of each row of `batch` to `groups`, with
  // `columns` as the key. New keys get new groups. On error, returns the
  // index in `columns` of the bad column. A column is bad if it cannot be a
  // key, or if its kind changed since an earlier batch.
  std::optional<uint32_t> FindOrCreate(const RowBatch& batch,
                                       const std::vector<uint32_t>& columns,
                                       uint32_t* groups);

  uint32_t size() const { return static_cast<uint32_t>(hashes_.size()); }
  void Clear();

  // Reserves `words` zeroed words after each group's keys for the caller to
  // store per-group data. Must be called before the first batch.
  void set_payload_words(uint32_t words) { payload_words_ = words; }
  // The payload of `group`. Consecutive groups' payloads are `row_stride()`
  // words apart.
  int64_t* payload(uint32_t group) {
    return group_rows_.data() + uint64_t{group} * stride_ + stride_ -
           payload_words_;
  }
  uint32_t row_stride() const { return stride_; }

  // Key `k` of `group` as stored: integers as int64, doubles as their bits
  // and strings as their interned id.
  int64_t Key(uint32_t group, uint32_t k) const {
    return group_rows_.data()[uint64_t{group} * stride_ + 1 + k];
  }

  // Whether key `k` of `group` is null.
  bool IsNull(uint32_t group, uint32_t k) const {
    auto nulls =
        static_cast<uint64_t>(group_rows_.data()[uint64_t{group} * stride_]);
    return (nulls >> k) & 1;
  }

 private:
  // Fast path for a single key column with no nulls. It compares the key
  // values directly.
  void FindValues(uint32_t count, uint32_t* groups);
  // Adds a group for the key of `row` and returns its number.
  uint32_t Add(uint32_t row, uint64_t hash);
  uint64_t Hash(uint32_t row) const;
  bool Equal(uint32_t group, uint32_t row) const;
  // Puts `group` in the first free slot at or after its hash's slot.
  void Place(uint32_t group);
  void Grow(uint32_t groups);

  // The hash table. An empty slot is 0. A used slot holds the top 16 bits of
  // the group's hash and the group number plus one.
  FlexVector<uint64_t> slots_;

  // The hash of each group.
  FlexVector<uint64_t> hashes_;

  // Each group's null bits, then its keys, then its payload. Each group takes
  // `stride_` words. Keeping them together means comparing a group reads one
  // cache line.
  FlexVector<int64_t> group_rows_;
  uint32_t stride_ = 0;

  // The number of payload words per group.
  uint32_t payload_words_ = 0;

  // The kind of each key column.
  std::vector<KeyKind> kinds_;

  // The current batch's keys, one vector per column.
  std::vector<FlexVector<int64_t>> keys_;

  // The current batch's null bits, one word per row.
  FlexVector<uint64_t> nulls_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_GROUP_TABLE_H_
