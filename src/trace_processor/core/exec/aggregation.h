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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_AGGREGATION_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_AGGREGATION_H_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

// One aggregate an operator computes over the rows of a group.
struct AggregateCall {
  enum class Function : uint8_t { kCount, kSum };
  Function function = Function::kCount;
  // Unused by kCount.
  uint32_t column = 0;
};

// A sum over some rows, and how many of them held a value: a sum which no
// row added to is null. Add and Subtract return false on overflow.
struct Sum {
  int64_t sum;
  int64_t holding;

  // What `row` adds to a sum of the column `reader` reads.
  static PERFETTO_ALWAYS_INLINE Sum Of(const FlatColumnReader<int64_t>& reader,
                                       uint32_t row) {
    Sum value{};
    value.holding = reader.Read(row, &value.sum);
    return value;
  }

  PERFETTO_ALWAYS_INLINE bool Add(const Sum& other) {
    holding += other.holding;
    return base::CheckedAdd(sum, other.sum, &sum);
  }

  PERFETTO_ALWAYS_INLINE bool Subtract(const Sum& other) {
    holding -= other.holding;
    // Negating the minimum Int64 value would itself overflow. Subtract it
    // directly only when the result is representable.
    if (other.sum == std::numeric_limits<int64_t>::min()) {
      if (sum >= 0) {
        return false;
      }
      sum -= other.sum;
      return true;
    }
    return base::CheckedAdd(sum, -other.sum, &sum);
  }
};

// The count and sums of some rows.
struct AggregateTotals {
  int64_t count = 0;
  std::vector<Sum> sums;

  void Clear() {
    count = 0;
    std::fill(sums.begin(), sums.end(), Sum());
  }
};

class Aggregation;

// The aggregates of some output rows, stored as the columns they are
// emitted in.
class AggregateResults {
 public:
  // Makes room for `rows` rows, keeping those already set.
  void Resize(uint32_t rows);

  void SetCount(uint32_t row, int64_t count) { counts_[row] = count; }
  void SetSum(uint32_t row, uint32_t sum, const Sum& total) {
    sums_[sum].values[row] = total.sum;
    sums_[sum].present.change(row, total.holding > 0);
  }
  void Set(uint32_t row, const AggregateTotals& totals) {
    SetCount(row, totals.count);
    for (uint32_t i = 0; i < sums_.size(); ++i) {
      SetSum(row, i, totals.sums[i]);
    }
  }

 private:
  friend class Aggregation;

  struct SumColumn {
    FlexVector<int64_t> values;
    BitVector present;
  };
  FlexVector<int64_t> counts_;
  std::vector<SumColumn> sums_;
};

// The aggregates an operator computes: reads the columns they sum and lays
// their results out as one Int64 column each.
class Aggregation {
 public:
  using Readers = base::SmallVector<FlatColumnReader<int64_t>, 4>;

  explicit Aggregation(const std::vector<AggregateCall>& calls);

  // How many of the aggregates are sums. Totals and readers hold one entry
  // per sum, in the order the aggregates list them.
  uint32_t sums() const { return static_cast<uint32_t>(sum_columns_.size()); }

  AggregateTotals MakeTotals() const;
  AggregateResults MakeResults() const;

  // Whether every summed column of `in` is Int64, as Read requires.
  bool CanRead(const RowBatch& in) const;
  // Appends one reader per sum.
  void Read(const RowBatch& in, Readers* readers) const;

  // Appends one column per aggregate, viewing `results`.
  void AddColumns(const AggregateResults& results, RowBatch* out) const;

 private:
  std::vector<AggregateCall::Function> functions_;
  // The position among the sums of each aggregate which is one.
  std::vector<uint32_t> sum_index_;
  std::vector<uint32_t> sum_columns_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_AGGREGATION_H_
