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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_PARKED_ROWS_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_PARKED_ROWS_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

// Steps which reorder rows never hold them. Before a reorder, ParkRows holds
// the columns still needed after it in one of the run's stores, and numbers
// each row in that store; the reorder orders those numbers alone. After it,
// FetchRows reads a parked column back by them, only where the column is
// read. Every column is held once, by its store, however many reorders the
// rows go through.

namespace perfetto::trace_processor::core::exec {

class Context;
class RowStore;

struct ParkRowsSpec {
  // The store to park in: Context::Store(slot).
  uint32_t slot = 0;
  // The columns to park, in the order the store holds them.
  std::vector<uint32_t> columns;
};

// Parks `columns` of each batch in a store, and appends a Uint32 column
// holding each row's number in the store. The batch is otherwise unchanged.
// The store is emptied when a run starts and before every run after.
class ParkRows final : public Transform {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns* out) override {
    for (uint32_t& column : spec_.columns) {
      out->reads.push_back(&column);
    }
    out->added = 1;
  }

  explicit ParkRows(ParkRowsSpec);
  ~ParkRows() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  bool Process(RowBatch& batch, OperatorState& state) const override;
  base::Status status(const OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  struct State;

  ParkRowsSpec spec_;
};

struct FetchRowsSpec {
  // The store to read: Context::Store(slot).
  uint32_t slot = 0;
  // The Uint32 column holding each row's number in the store.
  uint32_t row_column = 0;
  // The store's columns to read, appended to the batch in this order.
  std::vector<uint32_t> columns;
};

// Appends to each batch the values `columns` of a store hold for each row's
// number in it. Rows numbered one after another, as a reorder leaves rows
// which were already in order, are viewed where the store holds them, without
// copying.
class FetchRows final : public Transform {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns* out) override {
    out->reads = {&spec_.row_column};
    out->added = static_cast<uint32_t>(spec_.columns.size());
  }

  explicit FetchRows(FetchRowsSpec);
  ~FetchRows() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  bool Process(RowBatch& batch, OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  struct State;

  FetchRowsSpec spec_;
};

// Lays a batch's columns out again: column i of the output is column
// `columns[i]` of the input, with the same rows. After rows were parked and
// fetched, puts each column back where the rest of the plan expects it.
class SelectColumns final : public Operator {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit SelectColumns(std::vector<uint32_t> columns);
  ~SelectColumns() override;

  void DescribeColumns(StepColumns* out) override {
    out->keeps = StepColumns::Keeps::kNone;
    for (uint32_t& column : columns_) {
      out->reads.push_back(&column);
    }
    out->added = static_cast<uint32_t>(columns_.size());
  }
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  std::vector<uint32_t> columns_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_PARKED_ROWS_H_
