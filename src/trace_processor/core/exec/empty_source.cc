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

#include "src/trace_processor/core/exec/empty_source.h"

#include <memory>

#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

EmptySource::EmptySource() = default;
EmptySource::~EmptySource() = default;

std::unique_ptr<OperatorState> EmptySource::MakeState(Context&) const {
  return std::make_unique<OperatorState>();
}

bool EmptySource::GetData(RowBatch& out, OperatorState&) const {
  out.Reset();
  return false;
}

}  // namespace perfetto::trace_processor::core::exec
