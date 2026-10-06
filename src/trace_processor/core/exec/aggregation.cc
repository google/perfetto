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

#include "src/trace_processor/core/exec/aggregation.h"

#include <cstdint>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

void AggregateResults::Resize(uint32_t rows) {
  counts_.resize(rows);
  for (SumColumn& sum : sums_) {
    sum.values.resize(rows);
    sum.present.resize(rows);
  }
}

Aggregation::Aggregation(const std::vector<AggregateCall>& calls) {
  for (const AggregateCall& call : calls) {
    functions_.push_back(call.function);
    sum_index_.push_back(sums());
    if (call.function == AggregateCall::Function::kSum) {
      sum_columns_.push_back(call.column);
    }
  }
}

AggregateTotals Aggregation::MakeTotals() const {
  AggregateTotals totals;
  totals.sums.resize(sums());
  return totals;
}

AggregateResults Aggregation::MakeResults() const {
  AggregateResults results;
  results.sums_.resize(sums());
  return results;
}

bool Aggregation::CanRead(const RowBatch& in) const {
  for (uint32_t column : sum_columns_) {
    const ColumnView& view = in.column(column);
    if (view.kind() != ColumnView::Kind::kFlat || !view.type().Is<Int64>()) {
      return false;
    }
  }
  return true;
}

void Aggregation::Read(const RowBatch& in, Readers* readers) const {
  for (uint32_t column : sum_columns_) {
    readers->emplace_back(in.column(column));
  }
}

void Aggregation::AddColumns(const AggregateResults& results,
                             RowBatch* out) const {
  for (uint32_t a = 0; a < functions_.size(); ++a) {
    if (functions_[a] == AggregateCall::Function::kSum) {
      const AggregateResults::SumColumn& sum = results.sums_[sum_index_[a]];
      out->AddColumn(ColumnView::Reference(StorageType{Int64{}},
                                           sum.values.data(), &sum.present));
    } else {
      out->AddColumn(
          ColumnView::Reference(StorageType{Int64{}}, results.counts_.data()));
    }
  }
}

}  // namespace perfetto::trace_processor::core::exec
