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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_SINK_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_SINK_H_

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// The end a plan is pushed into: a step told about batches rather than
// asking for them.
class Sink {
 public:
  virtual ~Sink();
  Sink(const Sink&) = delete;
  Sink& operator=(const Sink&) = delete;

  virtual base::Status Consume(RowBatch& batch, OperatorState& state) const = 0;

  virtual base::Status Finish(OperatorState& state) const = 0;

 protected:
  Sink() = default;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_SINK_H_
