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

// Numbers the distinct keys of rows in the order they are first seen. As in
// DuckDB's aggregate hash table, each slot holds the top bits of its group's
// hash, so a probe compares keys only when those match, and a group's keys
// are stored together.
//
// Integers of every width agree with each other, a double is its bits and a
// string its interned id. Nulls agree with each other and with nothing else.
// A column must hold the same kind of key in every batch.
class GroupTable {
 public:
  // Writes the group of each row of `batch` by its keys in `columns` to
  // `groups`, adding new ones. Returns the position in `columns` of one which
  // cannot be a key or whose kind changed.
  std::optional<uint32_t> FindOrCreate(const RowBatch& batch,
                                       const std::vector<uint32_t>& columns,
                                       uint32_t* groups);

  uint32_t size() const { return static_cast<uint32_t>(hashes_.size()); }
  void Clear();

 private:
  enum class Kind : uint8_t { kInteger, kDouble, kString };

  // Reads key column `k` of the batch into `keys_[k]`, marking its nulls in
  // `nulls_`.
  std::optional<Kind> Load(const ColumnView& column,
                           const Selection& selection,
                           uint32_t k,
                           uint32_t count);
  // Finds or adds each row's group when there is one key column and it has
  // no nulls, comparing the keys themselves.
  void FindValues(uint32_t count, uint32_t* groups);
  // Adds the group of the keys of `row`, returning its number.
  uint32_t Add(uint32_t row, uint64_t hash);
  uint64_t Hash(uint32_t row) const;
  bool Equal(uint32_t group, uint32_t row) const;
  // Puts `group` in the first free slot from its hash's.
  void Place(uint32_t group);
  void Grow(uint32_t groups);

  // 0 if empty, else the top 16 bits of the group's hash and the group + 1.
  FlexVector<uint64_t> slots_;

  // By group: its hash, then `stride_` words apart which of its keys are
  // null and the keys themselves, together so a compare reads one line.
  FlexVector<uint64_t> hashes_;
  FlexVector<int64_t> group_rows_;
  uint32_t stride_ = 0;
  std::vector<Kind> kinds_;

  // The batch's keys a column at a time, and which are null.
  std::vector<FlexVector<int64_t>> keys_;
  FlexVector<uint64_t> nulls_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_GROUP_TABLE_H_
