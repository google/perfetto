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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_HASH_AGGREGATE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_HASH_AGGREGATE_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/aggregate_function.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

struct HashAggregateSpec {
  // With no key columns, all rows are in one group, and that group is output
  // as a row even if there are no input rows.
  std::vector<uint32_t> key_columns;
  std::vector<AggregateCall> aggregates;
};

// Groups rows by their keys and outputs one row per group. Each output row has
// the keys, then one column per aggregate. Groups are output in the order
// they were first seen.
//
// Input rows are not stored. Each row is added to its group's aggregate
// state, which is stored next to the group's keys. Memory use grows with the
// number of groups, not the number of rows.
class HashAggregate : public Breaker {
 public:
  explicit HashAggregate(HashAggregateSpec);
  ~HashAggregate() override;

 private:
  struct State : Breaker::State {
    ~State() override;
    void Reset() override;

    GroupTable groups;

    // With no keys, the aggregate state of the single group.
    FlexVector<int64_t> total;

    // Whether any input row was read.
    bool consumed = false;

    AggregateInputLoader input;

    // For each aggregate, which groups have seen a non-null value.
    std::vector<SeenBytes> seen;

    // The type of each key column, taken from the first batch. The key values
    // are stored in the group table.
    std::vector<StorageType> key_types;

    // The group of each row of the current batch.
    FlexVector<uint32_t> row_groups;

    uint32_t emitted = 0;
    FlexVector<uint32_t> served_rows;
  };

  std::unique_ptr<Breaker::State> CreateState() const override;
  bool Consume(const RowBatch& in, Breaker::State& state) const override;
  bool Finalize(Breaker::State& state) const override;
  bool Serve(RowBatch& out, Breaker::State& state) const override;
  // Returns `base` such that group g's state for aggregate a is at
  // `base + offsets_[a] + g * stride`.
  int64_t* StateBase(State&, uint64_t* stride) const;
  uint32_t GroupCount(const State&) const;

  HashAggregateSpec spec_;
  std::vector<std::unique_ptr<AggregateFunction>> functions_;
  // The offset of each aggregate's state within a group's payload.
  std::vector<uint32_t> offsets_;
  uint32_t state_words_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_HASH_AGGREGATE_H_
