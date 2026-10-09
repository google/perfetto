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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_FLATTEN_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_FLATTEN_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/aggregate_function.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

struct IntervalFlattenSpec {
  uint32_t ts_column = 0;
  uint32_t dur_column = 0;
  std::vector<uint32_t> key_columns;
  // As GroupedSort appends. Unused without keys.
  uint32_t group_column = 0;
  std::vector<AggregateCall> aggregates;
};

// INTERVAL FLATTEN. The input must be grouped by the keys and ordered by ts
// within each group; the output is too, followed by a group number column.
// A row of no width becomes a segment of no width which also counts the rows
// spanning it. Completed segments are emitted in bounded batches; only active
// intervals and the keys needed by the current call are retained.
//
// Aggregates are computed with the shared functions a batch of segments at a
// time: each interval's state is kept in a slot of its own, combined into a
// tree over the segments it was live in, so any function works.
class IntervalFlatten : public Operator {
 public:
  explicit IntervalFlatten(IntervalFlattenSpec);
  ~IntervalFlatten() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  OpResult Finish(RowBatch&, OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  // A live interval: when it ends, the slot holding its aggregates' states,
  // and the first segment of the output batch it's live in.
  struct Live {
    int64_t end;
    uint32_t slot;
    uint32_t first;
  };
  // An interval's slot and the segments of the batch it was live in.
  struct Range {
    uint32_t slot;
    uint32_t first;
    uint32_t end;
  };

  struct State : OperatorState {
    State() : OperatorState(ResetEachRun{}) {}
    ~State() override;
    void Reset() override;

    base::Status status = base::OkStatus();
    // What output columns are copied out to.
    Context* context = nullptr;
    // The first unconsumed row when Execute yields a full output batch.
    uint32_t input_row = 0;
    // input_row can still be zero when draining the preceding group yields.
    bool input_pending = false;
    uint32_t input_group = 0;
    uint32_t output_group = 0;
    bool in_group = false;
    // The next segment starts at cursor. input_ts is the last accepted input
    // timestamp; its points remain pending until all rows at that time arrive.
    int64_t cursor = 0;
    int64_t input_ts = 0;
    // How many intervals of width are live, and the points at input_ts.
    int64_t live = 0;
    FlexVector<Live> instants;
    // A min-heap on end.
    FlexVector<Live> ends;

    // Every aggregate's states, `stride` words a group: the tree's nodes,
    // then slots.
    FlexVector<int64_t> states;
    uint32_t stride = 0;
    uint32_t slots = 0;
    // Free slots, zeroed.
    FlexVector<uint32_t> free_slots;
    // The intervals ended since the batch's last aggregation; then, while
    // aggregating, those still live.
    FlexVector<Range> ranges;
    // The input rows added since the last aggregation (all from the input of
    // this call), and their slots.
    FlexVector<uint32_t> rows;
    FlexVector<uint32_t> row_slots;
    // The slots merged into the tree's nodes, or into and out of deltas.
    FlexVector<GroupMerge> merges;
    FlexVector<GroupMerge> subtracts;
    // By aggregate.
    std::vector<SeenBytes> seen_bytes;
    AggregateInputLoader input;

    // Keys for the current input batch plus one carried key from the previous
    // input batch. Groups can share an output batch without retaining earlier
    // input.
    RowStore key_rows;
    RowBatch retained;
    RowBatch saved_key;
    uint32_t key_row = 0;
    uint32_t first_key = 0;

    // At most kMaxBatchRows completed segments, reused after each yield.
    // Active intervals remain in the heap until their ends are reached.
    uint32_t segments = 0;
    uint32_t segment_capacity = 0;
    // The segments' columns, written straight into the buffers the batch
    // served holds: taken as its first segment is emitted.
    ColumnBuffer ts_buffer;
    ColumnBuffer dur_buffer;
    ColumnBuffer group_buffer;
    int64_t* segment_ts = nullptr;
    int64_t* segment_dur = nullptr;
    uint32_t* segment_groups = nullptr;
    FlexVector<uint32_t> segment_key_rows;
    // By aggregate, the buffer the batch being served has its values in.
    std::vector<ColumnBuffer> outputs;
    RowBatch served_keys;
  };

  bool RetainKeys(const RowBatch&, State&) const;
  bool AddInterval(State&, uint32_t row, int64_t start, int64_t length) const;
  uint32_t TakeSlot(State&) const;
  // Aggregates the batch, publishes any completed segments, or
  // propagates an error. `in` is the input batch, if any.
  OpResult Yield(RowBatch&, State&, OpResult result, const RowBatch* in) const;
  bool Apply(State&, const RowBatch* in) const;
  GroupStates States(State&, uint32_t i, bool nulls) const;
  bool AggregateDeltas(State&) const;
  void AggregateTree(State&, bool* overflow) const;

  // Completes the last input timestamp and sweeps to time. Passing the maximum
  // timestamp drains a group. False means a full output batch; retrying
  // resumes at the first segment which has not yet been emitted.
  bool Advance(State&, int64_t time) const;
  void Expire(State&, int64_t end) const;
  void End(State&, const Live&) const;
  bool Emit(State&, int64_t ts, int64_t dur) const;
  void GrowSegments(State&) const;
  void TakeSegmentBuffers(State&) const;

  IntervalFlattenSpec spec_;
  // By aggregate of the spec.
  std::vector<std::unique_ptr<AggregateFunction>> functions_;
  std::vector<uint32_t> offsets_;
  uint32_t stride_ = 0;
  // Whether every function can subtract, so AggregateDeltas may be used.
  bool subtract_ = false;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_FLATTEN_H_
