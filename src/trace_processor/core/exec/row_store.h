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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_

#include <cstdint>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

// The rows an operator keeps.
//
// A batch is a borrow: its values belong to the operator that handed it over
// and are good until that operator is asked for the next one. An operator
// that needs rows for longer than that -- every breaker, by definition --
// copies them here.
//
// This is the only way to keep rows. An operator holds a store or it holds
// nothing, so the memory a query needs at once is the sum of its stores, and
// finding it is reading the operators' member lists rather than hunting for
// whichever scratch vectors somebody declared. It is also what makes the
// lifetime answerable: a store dies with its operator, so batches read from
// it are good for exactly as long as batches read from anything else.
//
// The copy is not the price of the rule -- it is the price of keeping rows at
// all, and it is paid once, here. What it buys back is that the columns land
// dense from row zero: a fold reads one flat span per column, and a computed
// column can sit beside them in the same index space.
//
// The copy is not the price of this rule -- it is the price of keeping rows
// at all, and it is paid once, here, rather than piecemeal into whatever
// scratch vectors an operator felt like declaring. What it buys back is that
// the columns land dense from row zero: a fold reads one flat span per
// column, and a computed column can sit beside them in the same index space.
class RowStore {
 public:
  // Copies every column of `batch` onto the end. The first append fixes the
  // shape; a later one which disagrees is an error rather than a surprise.
  base::Status Append(const RowBatch& batch);

  uint32_t size() const { return size_; }
  uint32_t column_count() const {
    return static_cast<uint32_t>(columns_.size());
  }
  StorageType type(uint32_t column) const { return columns_[column].type; }

  // A column read back whole. The rows are dense from zero, so a pass over a
  // column is a walk over an array and not a walk over a row view.
  Span<const int64_t> Int64Column(uint32_t column) const;

  // Points `batch` at `count` rows from `offset`, replacing whatever it held.
  // The caller is free to add its own columns afterwards.
  void View(RowBatch* batch, uint32_t offset, uint32_t count) const;

  // The same, for the rows `rows` picks out.
  void View(RowBatch* batch, Span<const uint32_t> rows) const;

  // Forgets the rows. The buffers stay, so a store which is filled again is
  // filled into what it already had.
  void Clear();

 private:
  struct Column {
    StorageType type{Uint32{}};
    std::variant<FlexVector<uint32_t>,
                 FlexVector<int32_t>,
                 FlexVector<int64_t>,
                 FlexVector<double>,
                 FlexVector<StringPool::Id>>
        values{FlexVector<uint32_t>()};
    BitVector validity;
    bool nullable = false;
  };

  base::Status AppendColumn(Column& into, const ColumnView& from, uint32_t n);
  ColumnView ViewOf(const Column& column) const;

  // Fixed in size once the first batch has arrived, so a view of one column
  // stays good while the others are appended to.
  std::vector<Column> columns_;
  uint32_t size_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
