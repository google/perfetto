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

#include "src/trace_processor/core/exec/aggregate_function.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

struct HashAggregateSpec {
  // With none, every row is in one group, which exists, and so comes out as
  // a row, even when no row does.
  std::vector<uint32_t> key_columns;
  std::vector<AggregateCall> aggregates;
};

// Groups rows by their keys and folds each group into one row as it reads
// them: the keys, then one Int64 column per aggregate. Rows are neither kept
// nor reordered: each is added into its group's state, which sits beside
// the group's keys, so memory follows the number of groups, not rows. Groups
// come out in the order they were first seen.
class HashAggregate : public Breaker {
 public:
  explicit HashAggregate(HashAggregateSpec);
  ~HashAggregate() override;

 private:
  struct State : Breaker::State {
    ~State() override;
    void Reset() override;

    GroupTable groups;
    // With no keys, the one group's state, and whether any row reached it.
    FlexVector<int64_t> total;
    bool consumed = false;
    AggregateInputLoader input;
    // By aggregate.
    std::vector<SeenBytes> seen;
    // A row for each group, in group order.
    RowStore key_rows;

    // Scratch for one input batch.
    FlexVector<uint32_t> row_groups;
    FlexVector<uint32_t> new_rows;
    RowBatch retained;

    uint32_t emitted = 0;
    FlexVector<uint32_t> served_rows;
    RowBatch served_keys;
  };

  std::unique_ptr<Breaker::State> CreateState() const override;
  bool Consume(const RowBatch& in, Breaker::State& state) const override;
  bool Finalize(Breaker::State& state) const override;
  bool Serve(RowBatch& out, Breaker::State& state) const override;
  // Where group g's state for aggregate a is: `base + offsets_[a] + g *
  // stride`.
  int64_t* StateBase(State&, uint64_t* stride) const;
  uint32_t GroupCount(const State&) const;

  HashAggregateSpec spec_;
  std::vector<std::unique_ptr<AggregateFunction>> functions_;
  // Where each aggregate's state starts in a group's payload.
  std::vector<uint32_t> offsets_;
  uint32_t state_words_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_HASH_AGGREGATE_H_
