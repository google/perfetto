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

#include "src/trace_processor/core/exec/parked_rows.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

struct ParkRows::State : OperatorState {
  State(Context& c, RowStore& s)
      : OperatorState(ResetEachRun{}), context(&c), store(&s) {}
  ~State() override;
  void Reset() override {
    store->Clear();
    status = base::OkStatus();
  }

  Context* context;
  RowStore* store;
  // Scratch: the columns of a batch parked.
  RowBatch parked;
  base::Status status = base::OkStatus();
};

ParkRows::State::~State() = default;

ParkRows::ParkRows(ParkRowsSpec spec) : spec_(std::move(spec)) {}
ParkRows::~ParkRows() = default;

std::unique_ptr<OperatorState> ParkRows::MakeState(Context& context) const {
  // A run starts with its store empty, whatever ran in the Context before.
  RowStore& store = context.Store(spec_.slot);
  store.Clear();
  return std::make_unique<State>(context, store);
}

bool ParkRows::Process(RowBatch& batch, OperatorState& state) const {
  auto& s = state.Cast<State>();
  RowStore& store = *s.store;
  uint32_t first = store.size();
  s.parked.Project(batch, spec_.columns);
  s.status = store.Append(s.parked, *s.context);
  // Not to keep the batch's buffers until the next one.
  s.parked.Reset();
  if (!s.status.ok()) {
    return false;
  }
  // Every row has a number, kept or not, so that a step reading the numbers
  // of all of a batch's rows, as FetchRows does, reads only rows in the
  // store. Rows dropped before parking are given number 0.
  ColumnBuffer buffer = s.context->TakeBuffer(sizeof(uint32_t));
  auto* numbers = buffer.chunk().Values<uint32_t>();
  const Selection& selection = batch.selection();
  if (selection.prefix() && batch.size() == batch.row_count()) {
    for (uint32_t i = 0; i < batch.size(); ++i) {
      numbers[i] = first + i;
    }
  } else {
    memset(numbers, 0, uint64_t{batch.row_count()} * sizeof(uint32_t));
    for (uint32_t i = 0; i < batch.size(); ++i) {
      numbers[selection[i]] = first + i;
    }
  }
  batch.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, numbers),
                  std::move(buffer));
  return true;
}

base::Status ParkRows::status(const OperatorState& state) const {
  return state.Cast<State>().status;
}

struct FetchRows::State : OperatorState {
  State(Context& c, RowStore& s) : context(&c), store(&s) {}
  ~State() override;

  Context* context;
  RowStore* store;
};

FetchRows::State::~State() = default;

FetchRows::FetchRows(FetchRowsSpec spec) : spec_(std::move(spec)) {}
FetchRows::~FetchRows() = default;

std::unique_ptr<OperatorState> FetchRows::MakeState(Context& context) const {
  return std::make_unique<State>(context, context.Store(spec_.slot));
}

bool FetchRows::Process(RowBatch& batch, OperatorState& state) const {
  auto& s = state.Cast<State>();
  RowStore& store = *s.store;
  uint32_t rows = batch.row_count();
  if (!batch.size()) {
    // No row is read, but the columns must still be there, holding what
    // they hold.
    const RowBatch& shape = store.shape();
    for (uint32_t column : spec_.columns) {
      batch.AddColumn(shape.column(column), shape.buffer(column));
    }
    return true;
  }
  const ColumnView& numbers = batch.column(spec_.row_column);
  PERFETTO_DCHECK(numbers.kind() == ColumnView::Kind::kFlat &&
                  numbers.type().Is<Uint32>() && !numbers.validity());
  const uint32_t* begin =
      static_cast<const uint32_t*>(numbers.data()) + numbers.start();
  store.GatherColumns(spec_.columns, Span<const uint32_t>(begin, begin + rows),
                      *s.context, &batch);
  return true;
}

SelectColumns::SelectColumns(std::vector<uint32_t> columns)
    : columns_(std::move(columns)) {}
SelectColumns::~SelectColumns() = default;

OpResult SelectColumns::Execute(const RowBatch& in,
                                RowBatch& out,
                                OperatorState&) const {
  out.Project(in, columns_);
  return OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
