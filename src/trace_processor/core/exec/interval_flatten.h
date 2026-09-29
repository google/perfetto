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

#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

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
// spanning it.
class IntervalFlatten : public Breaker {
 public:
  explicit IntervalFlatten(IntervalFlattenSpec);
  ~IntervalFlatten() override;
  BatchPreference batch_preference() const override {
    return BatchPreference::kThroughput;
  }

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

  struct State : Breaker::State {
    ~State() override;

    uint32_t group = 0;
    uint32_t group_number = 0;
    bool in_group = false;
    int64_t previous = 0;
    int64_t time = 0;
    Totals live;
    Totals instant;
    // A min-heap on end.
    FlexVector<Live> ends;
    // Sum values of live rows, by slot.
    uint32_t slots = 0;
    FlexVector<Sum> slot_sums;
    FlexVector<uint32_t> free_slots;

    RowStore key_rows;
    RowBatch retained;

    // The segments, a column each. All are `segment_capacity` long, of which
    // the first `segments` are used.
    uint32_t segments = 0;
    uint32_t segment_capacity = 0;
    FlexVector<int64_t> segment_ts;
    FlexVector<int64_t> segment_dur;
    FlexVector<uint32_t> segment_groups;
    FlexVector<uint32_t> segment_key_rows;
    uint32_t key_row = 0;
    // What every kCount aggregate holds.
    FlexVector<int64_t> segment_counts;
    struct SegmentSums {
      FlexVector<int64_t> values;
      BitVector present;
    };
    std::vector<SegmentSums> segment_sums;
    uint32_t served = 0;
    RowBatch served_keys;
  };

  std::unique_ptr<Breaker::State> CreateState() const override;
  bool Consume(const RowBatch& in, Breaker::State& state) const override;
  bool Finalize(Breaker::State& state) const override;
  bool Serve(RowBatch& out, Breaker::State& state) const override;
  void Reset(Breaker::State& state) const override;

  void Advance(State&, int64_t time) const;
  void EmitInstant(State&) const;
  void EndGroup(State&) const;
  void Emit(State&, int64_t ts, int64_t dur, bool with_instant) const;
  void GrowSegments(State&) const;

  IntervalFlattenSpec spec_;
  std::vector<uint32_t> sum_index_;
  uint32_t sums_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_FLATTEN_H_
