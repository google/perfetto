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

// Sources for a single equality on a column whose search is fixed by its
// kind, so a run does one search with no dispatch. They serve hot lookups like
// id = ? that run once per row of another table. Like SortedScan, they find
// the same rows as the dataframe interpreter.

namespace perfetto::trace_processor::core::exec {

// State shared by the scans below.
struct EqScanState : OperatorState {
  ~EqScanState() override;
  RangeReader reader;
  const Context* context = nullptr;
  bool searched = false;
};

// = on an Id column.
class IdEqScan final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  // `value` is the index of the run's filter value holding the id.
  IdEqScan(std::vector<std::shared_ptr<const dataframe::Column>> columns,
           uint32_t row_count,
           uint32_t value);
  ~IdEqScan() override;

  std::unique_ptr<OperatorState> MakeState(Context& context) const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;

  Range Find(const Context& context) const;

 private:
  std::vector<std::shared_ptr<const dataframe::Column>> columns_;
  uint32_t row_count_;
  uint32_t value_;
};

// = on a SetId sorted Uint32 column.
class SetIdEqScan final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  // `value` is the index of the run's filter value compared with `searched`.
  SetIdEqScan(std::vector<std::shared_ptr<const dataframe::Column>> columns,
              uint32_t row_count,
              std::shared_ptr<const dataframe::Column> searched,
              uint32_t value);
  ~SetIdEqScan() override;

  std::unique_ptr<OperatorState> MakeState(Context& context) const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;

  Range Find(const Context& context) const;

 private:
  std::vector<std::shared_ptr<const dataframe::Column>> columns_;
  uint32_t row_count_;
  std::shared_ptr<const dataframe::Column> searched_;
  uint32_t value_;
};

// = on a column with SmallValueEq storage.
class SmallValueEqScan final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  // `value` is the index of the run's filter value compared with `searched`.
  SmallValueEqScan(
      std::vector<std::shared_ptr<const dataframe::Column>> columns,
      uint32_t row_count,
      std::shared_ptr<const dataframe::Column> searched,
      uint32_t value);
  ~SmallValueEqScan() override;

  std::unique_ptr<OperatorState> MakeState(Context& context) const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;

  Range Find(const Context& context) const;

 private:
  std::vector<std::shared_ptr<const dataframe::Column>> columns_;
  uint32_t row_count_;
  std::shared_ptr<const dataframe::Column> searched_;
  uint32_t value_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_EQ_SCANS_H_
