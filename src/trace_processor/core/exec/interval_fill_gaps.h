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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_FILL_GAPS_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_FILL_GAPS_H_

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

struct IntervalFillGapsSpec {
  // An output column. Input rows take its value from the input. Filler rows
  // take it from the first of these that applies:
  //  - the background column with the same name;
  //  - the lane's PER value, if it is a PER column;
  //  - null.
  struct Output {
    static constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

    // Column index in the input.
    uint32_t input = 0;
    // Index in the background's retained columns, or kNone if it does not have
    // this column.
    uint32_t background = kNone;
    // Type of the null column used if no input batch was seen.
    StorageType fallback{Int64{}};
    // Column name, used in error messages.
    std::string name;
  };

  // The input. A dur of -1 lasts forever.
  IntervalLaneSpec input;

  // The background, read whole before the input. Its key columns are either
  // none or those matching the input's, in the same order.
  CollectedInputSpec background;

  std::vector<Output> outputs;
};

// INTERVAL FILL GAPS. Outputs every input row unchanged. Also outputs a
// filler row for each part of the background that no input row in the same
// lane covers.
//
// A lane is the set of rows with the same PER values. There are two modes:
//  - The background has the PER columns, or there are no PER columns. Each
//    background row belongs to one lane. A lane with no input is filled
//    completely. Input in a lane the background does not have gets no
//    fillers.
//  - The background has no PER columns but the input does. Every input lane
//    is filled against the whole background. Fillers get the lane's PER
//    values.
//
// Background rows in the same lane may not overlap. Fillers never have zero
// duration. Output rows are in no particular order.
class IntervalFillGaps : public Operator {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns* out) const override {
    out->keeps = StepColumns::Keeps::kNone;
    DescribeLaneReads(spec_.input, out);
    out->reads.insert(out->reads.end(), input_columns_.begin(),
                      input_columns_.end());
    out->added = static_cast<uint32_t>(spec_.outputs.size());
  }

  explicit IntervalFillGaps(IntervalFillGapsSpec);
  ~IntervalFillGaps() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  OpResult Finish(RowBatch&, OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  friend class PipelineFormatter;

  struct State;

  // Where a filler takes an output column from.
  enum class FillerSource : uint8_t { kTs, kDur, kBackground, kLaneKey, kNull };
  struct FillerColumn {
    FillerSource source;
    // The background's retained column, or the index of the PER column.
    uint32_t index;
  };

  // Reads the background on first call. Returns false on error.
  bool EnsureBackgroundRead(State&) const;
  base::Status ReadBackground(State&) const;

  // Finds the gaps left by the rows of one input batch.
  base::Status Cover(const RowBatch&, State&) const;

  // Starts lane `lane`, whose first row is row `row` of the input batch.
  void StartLane(uint32_t lane, uint32_t row, State&) const;

  // Writes the next batch of filler rows to `out`.
  OpResult ServeFillers(RowBatch& out, State&) const;

  IntervalFillGapsSpec spec_;

  // Whether the input's lanes are filled against the whole background, and
  // so fillers take their PER values from the input.
  bool broadcast_;

  // Whether lanes are matched across the input and background by key.
  bool shared_;

  // The input column of each output, and where fillers take it from.
  std::vector<uint32_t> input_columns_;
  std::vector<FillerColumn> filler_columns_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_FILL_GAPS_H_
