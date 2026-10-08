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
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

struct IntervalFillGapsSpec {
  // One output column: where an input row's value and a filler's value come
  // from. A filler's bounds are its own, and a PER column the background
  // lacks takes the lane's value; anything else from neither side is null.
  struct Output {
    // Position in an input batch, or none for a background-only column.
    std::optional<uint32_t> input;
    // Position in a background batch, or none for an input-only column.
    std::optional<uint32_t> background;
    // The type of a null column when neither side has shown one, as when
    // there are no input rows.
    StorageType fallback{Int64{}};
    // For error messages.
    std::string name;
  };

  // Flat Int64 columns of the input.
  uint32_t ts_column = 0;
  uint32_t dur_column = 0;
  std::vector<uint32_t> key_columns;

  // Read in full before any input row passes through. Its PER columns are
  // either all of `key_columns`' counterparts, in the same order, or none.
  const Source* background = nullptr;
  uint32_t background_ts_column = 0;
  uint32_t background_dur_column = 0;
  std::vector<uint32_t> background_key_columns;

  std::vector<Output> outputs;
};

// INTERVAL FILL GAPS. Every input row passes through unchanged, and each span
// of a background row its lane's input does not cover becomes a filler row.
//
// Lanes come from the PER columns, in one of three ways (see LaneMode): with
// no PER there is one lane; a background with the PER columns names its own
// lanes; and a background without them applies to every lane of the input.
//
// An input row with a null ts or dur covers nothing; a dur of -1 covers to
// the end of time. A ts below zero is refused on either side. Input rows may
// overlap. Background rows may not overlap within a lane. A filler never has
// zero width. Output is in no order: the input first, then the fillers.
//
// See the .cc file for how the fillers are found.
class IntervalFillGaps : public Operator {
 public:
  explicit IntervalFillGaps(IntervalFillGapsSpec);
  ~IntervalFillGaps() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  OpResult Finish(RowBatch&, OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  struct State;

  // How rows are split into lanes, each filled on its own.
  enum class LaneMode : uint8_t {
    // No PER: every row is in one lane, filled even when there is no input.
    kOne,
    // PER, and the background has the PER columns: it names the lanes, so a
    // lane without input is filled whole, and input in a lane the background
    // does not name fills nothing.
    kByBackground,
    // PER, and the background does not have the PER columns: every lane of
    // the input is filled over the whole background, and fillers take the
    // lane's PER values from the input.
    kByInput,
  };

  // Reads the background unless it has been, returning whether all is well.
  bool EnsureBackgroundRead(State&) const;
  base::Status ReadBackground(State&) const;
  // Records the coverage of an input batch.
  base::Status Cover(const RowBatch&, State&) const;
  // Records the shapes of the outputs' columns in a batch from one side, the
  // first time it shows them, failing if one disagrees with the other side.
  base::Status RecordShapes(
      const RowBatch&,
      bool background,
      std::vector<std::optional<ColumnShape>>* shapes,
      const std::vector<std::optional<ColumnShape>>& other_side) const;
  // Finds every lane's fillers.
  void Fill(State&) const;
  // A null column shaped `like`, or of `fallback` while no batch has shown
  // a shape.
  ColumnView Nulls(const std::optional<ColumnShape>& like,
                   StorageType fallback) const;

  IntervalFillGapsSpec spec_;
  LaneMode mode_;
  // Positions in the retained background columns, by output; none where the
  // output does not read the background.
  std::vector<std::optional<uint32_t>> retained_of_output_;
  std::vector<uint32_t> retained_columns_;
  // The input PER column, by position in key_columns, each output repeats.
  std::vector<std::optional<uint32_t>> key_of_output_;

  // Backing for null columns, never written after construction.
  FlexVector<int64_t> zeros_;
  FlexVector<Variant> null_variants_;
  BitVector no_values_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_FILL_GAPS_H_
