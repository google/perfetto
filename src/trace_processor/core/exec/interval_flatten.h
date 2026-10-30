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
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/run_held.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

struct IntervalFlattenSpec {
  IntervalLaneSpec lanes;
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
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns* out) const override {
    out->keeps = StepColumns::Keeps::kNone;
    DescribeLaneReads(spec_.lanes, out);
    DescribeAggregateReads(spec_.aggregates, out);
    // The bounds, the keys, the aggregates and each row's group number.
    out->added = static_cast<uint32_t>(2 + spec_.lanes.key_columns.size() +
                                       spec_.aggregates.size() + 1);
  }

  explicit IntervalFlatten(IntervalFlattenSpec);
  ~IntervalFlatten() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  OpResult Finish(RowBatch&, OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  friend class PipelineFormatter;

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
    explicit State(const IntervalLaneSpec& lanes)
        : keys(this, lanes.key_columns) {}
    ~State() override;

    // Every aggregate's states: the tree's first nodes are there from the
    // start of every run, and zeroed as used.
    struct TreeStates : FlexVector<int64_t> {
      uint64_t first = 0;
      void EndRun() {
        EndRunOf(static_cast<FlexVector<int64_t>*>(this));
        resize(first);
      }
    };

    // What output columns are copied out to.
    Context* context = nullptr;
    // Words of state a group.
    uint32_t stride = 0;

    Held<base::Status> status{this};

    // Where the input is up to.
    // The first unconsumed row when Execute yields a full output batch.
    RunValue<uint32_t> input_row{this};
    // input_row can still be zero when draining the preceding group yields.
    RunValue<bool> input_pending{this, false};
    RunValue<uint32_t> input_group{this};
    RunValue<uint32_t> output_group{this};
    RunValue<bool> in_group{this, false};
    // The next segment starts at cursor. input_ts is the last accepted input
    // timestamp; its points remain pending until all rows at that time arrive.
    RunValue<int64_t> cursor{this};
    RunValue<int64_t> input_ts{this};
    // The keys of the input batch's lanes, and the current lane's handle.
    Held<LaneKeys> keys;
    RunValue<uint32_t> key_row{this};

    // The live intervals.
    // How many intervals of width are live, and the points at input_ts.
    RunValue<int64_t> live{this};
    Held<FlexVector<Live>> instants{this};
    // A min-heap on end.
    Held<FlexVector<Live>> ends{this};

    // The aggregates' states.
    // `stride` words a group: the tree's nodes, then slots.
    Held<TreeStates> states{this};
    RunValue<uint32_t> slots{this};
    // Free slots, zeroed.
    Held<FlexVector<uint32_t>> free_slots{this};
    // The intervals ended since the batch's last aggregation; then, while
    // aggregating, those still live.
    Held<FlexVector<Range>> ranges{this};
    // The slots merged into the tree's nodes, or into and out of deltas.
    Held<FlexVector<GroupMerge>> merges{this};
    Held<FlexVector<GroupMerge>> subtracts{this};
    // By aggregate.
    Held<RunEach<SeenBytes>> seen_bytes{this};

    // The output segments.
    // At most kMaxBatchRows completed segments, reused after each yield.
    // Active intervals remain in the heap until their ends are reached.
    RunValue<uint32_t> segments{this};
    RunValue<uint32_t> segment_capacity{this};
    // The segments' columns, written straight into the buffers the batch
    // served holds: taken as its first segment is emitted.
    Held<BoundsColumns> bounds{this};
    Held<ColumnBuffer> group_buffer{this};
    RunValue<uint32_t*> segment_groups{this, nullptr};
    Held<FlexVector<uint32_t>> segment_key_rows{this};
    // By aggregate, the buffer the batch being served has its values in.
    Held<RunEach<ColumnBuffer>> outputs{this};
    Held<RowBatch> served_keys{this};

    // Scratch for one call.
    // The input rows added since the last aggregation (all from the input of
    // this call), and their slots.
    CallScratch<FlexVector<uint32_t>> rows;
    CallScratch<FlexVector<uint32_t>> row_slots;
    CallScratch<AggregateInputLoader> input;
  };

  void AddInterval(State&, uint32_t row, int64_t start, int64_t end) const;
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
  // Whether the input has PER columns.
  bool keyed_;
  // By aggregate of the spec.
  std::vector<std::unique_ptr<AggregateFunction>> functions_;
  std::vector<uint32_t> offsets_;
  uint32_t stride_ = 0;
  // Whether every function can subtract, so AggregateDeltas may be used.
  bool subtract_ = false;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_FLATTEN_H_
