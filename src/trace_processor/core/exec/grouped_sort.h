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
#include <utility>
#include <vector>

#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_held.h"
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
};

// Groups rows by the group columns and stably sorts each group by the keys.
// Groups come out in the order they are first seen, not sorted.
//
// Also appends a Uint32 column with each row's group number. Later operators
// can use it to find group boundaries without comparing values.
//
// Group numbers come from a hash table. A counting sort by group number then
// brings each group's rows together. Rows are copied once, in the final
// order.
class GroupedSort : public Breaker {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns* out) const override {
    out->reads = spec_.groups;
    for (const SortKey& key : spec_.keys) {
      out->reads.push_back(key.column);
    }
    // Each row's group number.
    out->added = 1;
  }
  void SetUnreadColumns(std::vector<bool> unread) override {
    unread_ = std::move(unread);
  }

  explicit GroupedSort(GroupedSortSpec);
  ~GroupedSort() override;

 private:
  friend class PipelineFormatter;

  struct State : Breaker::State {
    ~State() override;

    // The rows, as they arrive, and the group of each.
    Held<SortedRows> rows{this};
    Held<GroupTable> group_table{this};
    // The group of each row, in input order.
    Held<FlexVector<uint32_t>> groups{this};

    // Grouping the sorted rows: where each group's rows go, and the rows in
    // group order.
    Held<FlexVector<uint32_t>> next{this};
    Held<FlexVector<uint32_t>> scratch{this};

    // The rows served, and the group of each, in output order.
    Held<FlexVector<uint32_t>> ordered_groups{this};
    RunValue<uint32_t> emitted{this};
  };

  std::unique_ptr<Breaker::State> CreateState() const override;
  bool Consume(const RowBatch& in, Breaker::State& state) const override;
  bool Finalize(Breaker::State& state) const override;
  bool Serve(RowBatch& out, Breaker::State& state) const override;

  GroupedSortSpec spec_;
  // Input columns no later step reads (see Operator::SetUnreadColumns).
  std::vector<bool> unread_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_GROUPED_SORT_H_
