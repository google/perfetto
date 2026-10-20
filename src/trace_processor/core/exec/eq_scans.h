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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_EQ_SCANS_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_EQ_SCANS_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/dataframe_windows.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/range.h"

// Sources reading the rows of a dataframe which one equality keeps, for the
// columns whose search is the same whatever the plan: each knows its column's
// type and comparison, so a run is one search with nothing dispatched, then
// batches pointing at the dataframe's own storage. These are the lookups a
// query can run millions of times, once per row of another table.
//
// The search runs when a run's first batch is asked for, comparing with one
// of the run's filter values (see Context::SetFilterValues), and finds exactly
// the rows the dataframe interpreter's would. Plans with more than one search,
// or a binary search, use SortedScan.

namespace perfetto::trace_processor::core::exec {

// What one run of an equality scan has found and read.
struct EqScanState : OperatorState {
  ~EqScanState() override;
  RangeReader reader;
  const Context* context = nullptr;
  // Whether this run's search has run.
  bool searched = false;
};

// = on an Id column: the row whose number is the value.
class IdEqScan final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  // Reads `columns` of a dataframe of `row_count` rows, at the row whose id is
  // filter value `value`.
  IdEqScan(std::vector<std::shared_ptr<const dataframe::Column>> columns,
           uint32_t row_count,
           uint32_t value);
  ~IdEqScan() override;

  std::unique_ptr<OperatorState> MakeState(Context& context) const override;
  const Layout& layout() const override { return layout_; }
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;

  // With every column read where it lies, a run finds at most one row and
  // needs nothing else: the columns are lent once, and each run only finds
  // the row.
  uint64_t needs() const override { return needs_; }
  void Lend(RowBatch& out, OperatorState& state) const override;
  bool FindRow(OperatorState& state, uint32_t* row) const override;

  // The rows a run with the values in `context` reads.
  Range Find(const Context& context) const;

 private:
  std::vector<std::shared_ptr<const dataframe::Column>> columns_;
  Layout layout_;
  uint32_t row_count_;
  uint32_t value_;
  uint64_t needs_ = kNeedsAll;
};

// = on a Uint32 column sorted by SetId: the run of rows starting at the row
// whose number is the value.
class SetIdEqScan final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  // Reads `columns` of a dataframe of `row_count` rows, at the rows whose
  // `searched` is filter value `value`.
  SetIdEqScan(std::vector<std::shared_ptr<const dataframe::Column>> columns,
              uint32_t row_count,
              std::shared_ptr<const dataframe::Column> searched,
              uint32_t value);
  ~SetIdEqScan() override;

  std::unique_ptr<OperatorState> MakeState(Context& context) const override;
  const Layout& layout() const override { return layout_; }
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;

  // The rows a run with the values in `context` reads.
  Range Find(const Context& context) const;

 private:
  std::vector<std::shared_ptr<const dataframe::Column>> columns_;
  Layout layout_;
  uint32_t row_count_;
  std::shared_ptr<const dataframe::Column> searched_;
  uint32_t value_;
};

// = on a column with SmallValueEq storage: at most one row, found by counting
// the values below the value.
class SmallValueEqScan final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  // Reads `columns` of a dataframe of `row_count` rows, at the row whose
  // `searched` is filter value `value`.
  SmallValueEqScan(
      std::vector<std::shared_ptr<const dataframe::Column>> columns,
      uint32_t row_count,
      std::shared_ptr<const dataframe::Column> searched,
      uint32_t value);
  ~SmallValueEqScan() override;

  std::unique_ptr<OperatorState> MakeState(Context& context) const override;
  const Layout& layout() const override { return layout_; }
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;

  // The rows a run with the values in `context` reads.
  Range Find(const Context& context) const;

 private:
  std::vector<std::shared_ptr<const dataframe::Column>> columns_;
  Layout layout_;
  uint32_t row_count_;
  std::shared_ptr<const dataframe::Column> searched_;
  uint32_t value_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_EQ_SCANS_H_
