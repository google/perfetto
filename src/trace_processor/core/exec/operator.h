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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_OPERATOR_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_OPERATOR_H_

#include <cstdint>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

enum class OpResult : uint8_t {
  // The batch continues to the next operator.
  kContinue,
  // The batch has no rows left; the rest of the pipeline skips it.
  kDrop,
};

// A streaming step in a pipeline. An operator may add columns or select rows,
// but it always updates one batch in place and produces at most one output
// batch for each input batch.
class Operator {
 public:
  virtual ~Operator();
  Operator(const Operator&) = delete;
  Operator& operator=(const Operator&) = delete;

  // Called once per execution, before the first batch, so an operator built
  // once per query plan can pick up values that change between executions.
  virtual void Open() {}

  virtual OpResult Execute(RowBatch& batch) = 0;

 protected:
  Operator() = default;
};

// Produces the batches a pipeline runs over.
class Source {
 public:
  virtual ~Source();
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;

  // Rewinds to the first batch so the source can be replayed.
  virtual void Reset() {}

  // The next batch, or null when exhausted. The batch stays owned by the
  // source and remains valid until the next Next() call, as do any row
  // indices borrowed from it.
  virtual RowBatch* Next() = 0;

  // Distinguishes normal exhaustion from an error after Next() returns no
  // batch. In-memory sources retain the default OK status.
  virtual base::Status status() const { return base::OkStatus(); }

 protected:
  Source() = default;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_OPERATOR_H_
