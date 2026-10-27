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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_GROUPED_SORT_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_GROUPED_SORT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/sorted_rows.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

struct GroupedSortSpec {
  // The columns to group by, as in GROUP BY. Nulls are equal to each other.
  // Each column must be Uint32, Int32, Int64, Double or String. There must
  // be at least one.
  std::vector<uint32_t> groups;
  // The keys to sort each group by. With no keys, each group keeps the input
  // order of its rows. The planner relies on this.
  std::vector<SortKey> keys;
  // The Uint32 column holding each row's number in the store it is parked in
  // (see ParkRows): set by the planner when it parks the rows.
  uint32_t numbers = 0;
};

// Groups rows parked in a store (see ParkRows) by the group columns and
// stably sorts each group by the keys. Groups come out in the order they are
// first seen, not sorted.
//
// Holds only the keys, each row's group and its number in the store, and
// outputs two Uint32 columns: the numbers, in grouped order, then each row's
// group number. Later operators can use the group numbers to find group
// boundaries without comparing values.
//
// Group numbers come from a hash table. A counting sort by group number then
// brings each group's rows together.
class GroupedSort : public Breaker {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns* out) override {
    out->keeps = StepColumns::Keeps::kRowNumbers;
    for (uint32_t& group : spec_.groups) {
      out->reads.push_back(&group);
    }
    for (SortKey& key : spec_.keys) {
      out->reads.push_back(&key.column);
    }
    out->numbers = &spec_.numbers;
    // Each row's group number.
    out->added = 1;
  }

  explicit GroupedSort(GroupedSortSpec);
  ~GroupedSort() override;

 private:
  friend class PipelineFormatter;

  struct State : Breaker::State {
    ~State() override;
    void Reset() override;

    GroupTable group_table;
    SortedRows rows;
    // The group of each row, in input order.
    FlexVector<uint32_t> groups;

    // The group of each row, in output order.
    FlexVector<uint32_t> ordered_groups;

    FlexVector<uint32_t> next;
    FlexVector<uint32_t> scratch;
    uint32_t emitted = 0;
  };

  std::unique_ptr<Breaker::State> CreateState() const override;
  bool Consume(const RowBatch& in, Breaker::State& state) const override;
  bool Finalize(Breaker::State& state) const override;
  bool Serve(RowBatch& out, Breaker::State& state) const override;

  GroupedSortSpec spec_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_GROUPED_SORT_H_
