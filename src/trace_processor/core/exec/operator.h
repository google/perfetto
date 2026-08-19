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
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

enum class OpResult : uint8_t {
  // The chunk continues to the next operator.
  kContinue,
  // The chunk has no rows left; the rest of the pipeline skips it.
  kDrop,
};

// A streaming step in a pipeline. An operator may add columns or select rows,
// but it always updates one chunk in place and produces at most one output
// chunk for each input chunk.
class Operator {
 public:
  virtual ~Operator();
  Operator(const Operator&) = delete;
  Operator& operator=(const Operator&) = delete;

  // Arms the operator for one execution, handing it that execution's filter
  // values. A tree is built once per query plan and reused, so anything that
  // changes between runs is picked up here rather than held.
  //
  // Returns false if what it picked up rules out every row, so that a query
  // that cannot match anything is answered without reading a chunk.
  virtual bool Open(ValueFetcher&) { return true; }

  virtual OpResult Execute(RowBatch& chunk) = 0;

 protected:
  Operator() = default;
};

// Produces the chunks a pipeline runs over.
class Source {
 public:
  virtual ~Source();
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;

  // Rewinds to the first chunk so the source can be replayed.
  virtual void Reset() {}

  // The next chunk, or null once exhausted. The chunk belongs to the source and
  // stays valid until the following Next() call on it, which is also how long
  // any row indices borrowed from it remain valid.
  virtual RowBatch* Next() = 0;

  // Distinguishes normal exhaustion from an error after Next() returns no
  // chunk. In-memory sources retain the default OK status.
  virtual base::Status status() const { return base::OkStatus(); }

 protected:
  Source() = default;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_OPERATOR_H_
