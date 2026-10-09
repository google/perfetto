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
  // The columns whose rows need only be together, in no order: as GROUP BY's
  // keys, where two rows holding no value agree. Any type; at least one.
  std::vector<uint32_t> groups;
  // What each group's rows are sorted by.
  std::vector<SortKey> keys;
};

// Puts rows sharing their group columns next to each other, each group's
// rows stably sorted by the keys, and appends each row's group number as a
// Uint32 column, so what follows finds group boundaries without comparing
// values. Groups need only be together, so they come in the order first seen
// rather than sorted: numbered by hashing their values, they are brought
// together by a counting pass over the order the keys give. Rows are copied
// once, in the final order.
class GroupedSort : public Breaker {
 public:
  explicit GroupedSort(GroupedSortSpec);
  ~GroupedSort() override;

 private:
  struct State : Breaker::State {
    ~State() override;
    void Reset() override;

    GroupTable group_table;
    SortedRows rows;
    // Each row's group, as held; then each row's group in the final order.
    FlexVector<uint32_t> groups;
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
