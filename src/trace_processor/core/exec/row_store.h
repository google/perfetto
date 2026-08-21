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
#include <memory>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

// Rows an operator keeps. A batch is a borrow, good until the next pull;
// anything needing rows for longer copies them here, so a query's memory is
// the sum of its stores.
class RowStore {
 public:
  // Copies every column onto the end. The first append fixes the shape.
  base::Status Append(const RowBatch& batch);

  uint32_t size() const { return size_; }
  uint32_t column_count() const {
    return static_cast<uint32_t>(columns_.size());
  }
  StorageType type(uint32_t column) const { return columns_[column].type; }
  bool is_variant(uint32_t column) const { return columns_[column].variant; }

  // A column read back whole, dense from row zero.
  Span<const int64_t> Int64Column(uint32_t column) const;
  Span<const Variant> VariantColumn(uint32_t column) const;

  // Points `batch` at rows of this store, replacing whatever it held.
  void View(RowBatch* batch, uint32_t offset, uint32_t count) const;
  void View(RowBatch* batch, Span<const uint32_t> rows) const;

  // Forgets the rows, keeping the buffers.
  void Clear();

 private:
  struct Data {
    std::variant<FlexVector<uint32_t>,
                 FlexVector<int32_t>,
                 FlexVector<int64_t>,
                 FlexVector<double>,
                 FlexVector<StringPool::Id>,
                 FlexVector<Variant>>
        values{FlexVector<uint32_t>()};
    BitVector validity;
  };
  struct Column {
    StorageType type{Uint32{}};
    bool variant = false;
    bool nullable = false;
    std::shared_ptr<Data> data = std::make_shared<Data>();
  };

  base::Status AppendColumn(Column&, const ColumnView&, uint32_t count);
  ColumnView ViewOf(const Column&) const;

  std::vector<Column> columns_;
  uint32_t size_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
