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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_EXPAND_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_EXPAND_H_

#include <cstdint>

#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/arena.h"

namespace perfetto::trace_processor::core::exec {

// Lays sparse columns (see ColumnView::Kind::kSparse) out with a slot per row,
// so that later steps read them as flat columns. Only the rows the batch keeps
// are given their values; the others read as zero.
class Expand final : public Transform {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit Expand(core::ArenaSpan<uint32_t> columns);

  void DescribeColumns(StepColumns* out) const override;
  bool Process(RowBatch& batch, OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  core::ArenaSpan<uint32_t> columns_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_EXPAND_H_
