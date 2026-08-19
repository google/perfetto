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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_OPERATOR_PLAN_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_OPERATOR_PLAN_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "perfetto/ext/base/small_vector.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"

namespace perfetto::trace_processor::core::dataframe {

// A query lowered for the operator executor.
//
// Flat and built from trivially copyable pieces, because SQLite owns the plan
// between xBestIndex and xFilter: it survives as bytes and the operator tree is
// rebuilt from it on the other side.
//
// Columns are addressed by their position in the row batch rather than by
// dataframe column. Position `kIdentityBatchColumn` is a synthetic row-identity
// column carrying no data, so every chunk has a selection the cursor can read
// row indices from even when no operator reads a column. `batch_columns` lists
// the dataframe columns at positions from `kFirstDataBatchColumn` onwards, and
// holds only the columns operators actually touch: output columns are read
// straight from storage by row index and never enter a batch.
struct OperatorPlan {
  static constexpr uint32_t kIdentityBatchColumn = 0;
  static constexpr uint32_t kFirstDataBatchColumn = kIdentityBatchColumn + 1;

  struct Filter {
    uint32_t batch_column = 0;
    // Where the caller supplies the value to compare against.
    uint32_t value_index = 0;
    StorageType storage{Id{}};
    Op op{Eq{}};

    bool operator==(const Filter& o) const {
      return batch_column == o.batch_column && value_index == o.value_index &&
             storage.index() == o.storage.index() && op.index() == o.op.index();
    }
  };
  static_assert(std::is_trivially_copyable_v<Filter>);

  // False when the plan uses an operation this lowering does not handle, in
  // which case the caller must run the query some other way. That set shrinking
  // is how this backend grows.
  bool supported = false;
  // The planner proved the result empty, so no rows need be examined.
  bool empty = false;
  uint32_t row_count = 0;
  base::SmallVector<uint32_t, 8> batch_columns;
  base::SmallVector<Filter, 8> filters;

  // Whether `o` describes the same work. A cursor re-prepared with an
  // unchanged plan can keep the operator tree it already built.
  bool operator==(const OperatorPlan& o) const {
    if (supported != o.supported || empty != o.empty ||
        row_count != o.row_count ||
        batch_columns.size() != o.batch_columns.size() ||
        filters.size() != o.filters.size()) {
      return false;
    }
    for (size_t i = 0; i < batch_columns.size(); ++i) {
      if (batch_columns[i] != o.batch_columns[i]) {
        return false;
      }
    }
    for (size_t i = 0; i < filters.size(); ++i) {
      if (!(filters[i] == o.filters[i])) {
        return false;
      }
    }
    return true;
  }
};

}  // namespace perfetto::trace_processor::core::dataframe

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_OPERATOR_PLAN_H_
