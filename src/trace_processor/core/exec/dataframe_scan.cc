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

#include "src/trace_processor/core/exec/dataframe_scan.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/dataframe_windows.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

DataframeScan::DataframeScan(
    std::vector<std::shared_ptr<const dataframe::Column>> columns,
    uint32_t row_count)
    : columns_(std::move(columns)), row_count_(row_count) {}

DataframeScan::~DataframeScan() = default;
DataframeScan::State::~State() = default;

std::unique_ptr<OperatorState> DataframeScan::MakeState(
    Context& context) const {
  auto state = std::make_unique<State>();
  state->reader.Init(columns_, context);
  state->reader.Start({0, row_count_});
  return state;
}

void DataframeScan::Rewind(OperatorState& state) const {
  state.Cast<State>().reader.Start({0, row_count_});
}

bool DataframeScan::GetData(RowBatch& out, OperatorState& state) const {
  return state.Cast<State>().reader.Read(out);
}

}  // namespace perfetto::trace_processor::core::exec
