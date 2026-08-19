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

#include "src/trace_processor/core/dataframe/operator_lowering.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <variant>

#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/logical_plan.h"
#include "src/trace_processor/core/dataframe/operator_plan.h"

namespace perfetto::trace_processor::core::dataframe {
namespace {

template <typename T>
constexpr size_t OpIndex() {
  return base::variant_index<logical::Operation, T>();
}

// Batch position for a dataframe column, appending it when first seen.
uint32_t BatchColumnFor(OperatorPlan& plan, uint32_t dataframe_column) {
  auto* it = std::find(plan.batch_columns.begin(), plan.batch_columns.end(),
                       dataframe_column);
  size_t offset;
  if (it != plan.batch_columns.end()) {
    offset = static_cast<size_t>(it - plan.batch_columns.begin());
  } else {
    plan.batch_columns.emplace_back(dataframe_column);
    offset = plan.batch_columns.size() - 1;
  }
  return OperatorPlan::kFirstDataBatchColumn + static_cast<uint32_t>(offset);
}

// The comparisons the filter operator implements.
bool IsSupportedComparison(Op op) {
  return op.Is<Eq>() || op.Is<Ne>() || op.Is<Lt>() || op.Is<Le>() ||
         op.Is<Gt>() || op.Is<Ge>();
}

// Whether the planner decided this filter is served by examining rows.
//
// The other strategies -- binary search, a SetId-sorted lookup, the
// SmallValueEq storage -- reach the answer without looking at most of the
// rows, and the operator executor has no equivalent yet. Scanning instead
// would turn a sub-linear lookup into a full pass, which is ruinous for a
// cursor executed once per row of a join, so those are not lowered here.
bool IsScanStrategy(const logical::FilterStrategy& strategy) {
  return std::holds_alternative<logical::RangeScan>(strategy) ||
         std::holds_alternative<logical::IndexListScan>(strategy);
}

bool IsSupportedFilter(const logical::Filter& filter) {
  // Id columns have no stored values and strings need interning, so neither is
  // lowered here, along with anything nullable.
  return filter.value_index.has_value() && IsSupportedComparison(filter.op) &&
         !filter.storage.Is<Id>() && !filter.storage.Is<String>() &&
         filter.nullability.Is<NonNull>() && IsScanStrategy(filter.strategy);
}

}  // namespace

OperatorPlan OperatorLowering::Lower(const LogicalPlan& plan) {
  OperatorPlan out;
  for (const logical::Operation& op : plan.ops) {
    switch (op.index()) {
      case OpIndex<logical::Scan>():
        out.row_count = std::get<logical::Scan>(op).rows.max;
        break;
      case OpIndex<logical::Filter>(): {
        const auto& filter = std::get<logical::Filter>(op);
        if (!IsSupportedFilter(filter)) {
          return OperatorPlan();
        }
        OperatorPlan::Filter lowered;
        lowered.batch_column = BatchColumnFor(out, filter.col);
        lowered.value_index = *filter.value_index;
        lowered.storage = filter.storage;
        lowered.op = filter.op;
        out.filters.emplace_back(lowered);
        break;
      }
      case OpIndex<logical::Empty>():
        out.empty = true;
        break;
      case OpIndex<logical::Output>():
        // Output columns are read from storage by row index rather than
        // flowing through the pipeline, so there is nothing to lower.
        break;
      default:
        // IndexFilter, Distinct, Reverse, Sort, MinMax and Limit have no
        // operator yet.
        return OperatorPlan();
    }
  }
  out.supported = true;
  return out;
}

}  // namespace perfetto::trace_processor::core::dataframe
