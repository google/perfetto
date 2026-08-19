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

#include "src/trace_processor/core/exec/pipeline.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Whether the batch still has rows for the consumer once every operator has
// seen it.
bool ExecuteOperators(RowBatch& batch,
                      const std::vector<std::unique_ptr<Operator>>& operators) {
  if (batch.size() == 0) {
    return false;
  }
  for (const std::unique_ptr<Operator>& op : operators) {
    if (op->Execute(batch) == OpResult::kDrop || batch.size() == 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

Operator::~Operator() = default;
Source::~Source() = default;

PullPipeline::PullPipeline(Source& source,
                           std::vector<std::unique_ptr<Operator>> operators)
    : source_(source), operators_(std::move(operators)) {}

PullPipeline::~PullPipeline() = default;

void PullPipeline::Reset() {
  source_.Reset();
  for (const std::unique_ptr<Operator>& op : operators_) {
    op->Open();
  }
}

RowBatch* PullPipeline::Next() {
  for (RowBatch* batch = source_.Next(); batch; batch = source_.Next()) {
    if (ExecuteOperators(*batch, operators_)) {
      return batch;
    }
  }
  return nullptr;
}

}  // namespace perfetto::trace_processor::core::exec
