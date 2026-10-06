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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_AGGREGATE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_AGGREGATE_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/aggregation.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

struct AggregateSpec {
  using Aggregate = AggregateCall;
  using Function = AggregateCall::Function;
  std::vector<uint32_t> key_columns;
  // As GroupBy appends. Unused without keys.
  uint32_t group_column = 0;
  std::vector<Aggregate> aggregates;
};

// Folds each group of rows into one row: the keys, then one Int64 column per
// aggregate. The input must be grouped by the keys, as GroupBy leaves it, so
// a group is a run of rows and only the one being read is held. Without keys
// every row is in one group, which exists even with no rows.
//
// A sum skips nulls and is null if it added nothing.
class Aggregate : public Operator {
 public:
  explicit Aggregate(AggregateSpec);
  ~Aggregate() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  OpResult Finish(RowBatch&, OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  struct State : OperatorState {
    State() : OperatorState(ResetEachRun{}) {}
    ~State() override;
    void Reset() override;

    base::Status status = base::OkStatus();
    bool finished = false;

    // The group being read, which later batches can add to.
    bool in_group = false;
    uint32_t group = 0;
    AggregateTotals totals;

    // Keys for the current input batch, after the key of a group carried
    // over from an earlier batch, whose buffers may since have been reused.
    RowStore key_rows;
    RowBatch retained;
    RowBatch saved_key;
    uint32_t key_row = 0;

    // The groups completed by this call.
    uint32_t completed = 0;
    FlexVector<uint32_t> completed_key_rows;
    AggregateResults completed_aggregates;
    RowBatch served_keys;
  };

  // Makes room for `groups` to complete in one call.
  void Reserve(State&, uint32_t groups) const;
  void Complete(State&) const;
  bool RetainKeys(const RowBatch&, State&) const;
  // Publishes any completed groups.
  OpResult Yield(RowBatch&, State&) const;

  AggregateSpec spec_;
  Aggregation aggregation_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_AGGREGATE_H_
