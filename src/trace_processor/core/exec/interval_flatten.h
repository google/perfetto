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
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

struct IntervalFlattenSpec {
  enum class Function : uint8_t { kCount, kSum };
  struct Aggregate {
    Function function = Function::kCount;
    // Unused by kCount.
    uint32_t column = 0;
  };
  uint32_t ts_column = 0;
  uint32_t dur_column = 0;
  std::vector<uint32_t> key_columns;
  // As GroupBy appends. Unused without keys.
  uint32_t group_column = 0;
  std::vector<Aggregate> aggregates;
};

// INTERVAL FLATTEN. The input must be grouped by the keys and ordered by ts
// within each group; the output is too, followed by a group number column.
// A row of no width becomes a segment of no width which also counts the rows
// spanning it. Completed segments are emitted in bounded batches; only active
// intervals and the keys needed by the current call are retained.
class IntervalFlatten : public Operator {
 public:
  explicit IntervalFlatten(IntervalFlattenSpec);
  ~IntervalFlatten() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  OpResult Finish(RowBatch&, OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  // A sum over some rows, and how many of them held a value.
  struct Sum {
    int64_t sum;
    int64_t holding;
  };
  struct Totals {
    int64_t count = 0;
    std::vector<Sum> sums;
  };
  struct Live {
    int64_t end;
    uint32_t slot;
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
    Totals live;
    Totals instant;
    // A min-heap on end.
    FlexVector<Live> ends;
    // Sum values of live rows, by slot.
    uint32_t slots = 0;
    FlexVector<Sum> slot_sums;
    FlexVector<uint32_t> free_slots;

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
    FlexVector<int64_t> segment_ts;
    FlexVector<int64_t> segment_dur;
    FlexVector<uint32_t> segment_groups;
    FlexVector<uint32_t> segment_key_rows;
    // What every kCount aggregate holds.
    FlexVector<int64_t> segment_counts;
    struct SegmentSums {
      FlexVector<int64_t> values;
      BitVector present;
    };
    std::vector<SegmentSums> segment_sums;
    RowBatch served_keys;
  };

  bool RetainKeys(const RowBatch&, State&) const;
  bool AddInterval(State&,
                   int64_t start,
                   int64_t length,
                   Span<const FlatColumnReader<int64_t>> sums) const;
  // Publishes any completed segments, or propagates an arithmetic error.
  OpResult Yield(RowBatch&, State&, OpResult result) const;

  static bool Add(State&, int64_t a, int64_t b, int64_t* out);
  // Completes the last input timestamp and sweeps to time. Passing the maximum
  // timestamp drains a group. False means an error or a full output batch;
  // retrying resumes at the first segment which has not yet been emitted.
  bool Advance(State&, int64_t time) const;
  bool Expire(State&, int64_t end) const;
  bool Emit(State&, int64_t ts, int64_t dur, bool with_instant) const;
  void GrowSegments(State&) const;

  IntervalFlattenSpec spec_;
  std::vector<uint32_t> sum_index_;
  uint32_t sums_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_FLATTEN_H_
