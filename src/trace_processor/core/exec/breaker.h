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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_BREAKER_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_BREAKER_H_

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/sink.h"

namespace perfetto::trace_processor::core::exec {

// A step which cannot answer until it has seen everything.
//
// A Sink while it fills and a Source once it is full, and both at once
// in the type, because that is what it is: every chunk goes in before any
// chunk comes out. An Operator cannot be this, because an
// Operator is handed a chunk and answers for that chunk; a sort, a subtree
// fold, or anything else whose first row depends on the last one has to be
// something else, and this is that something.
//
// Saying so in the type is the point. Where the breakers are is where a
// pipeline stops being a pipeline: the memory a query needs at once, and the
// latency before its first row, are both decided by them and by nothing else.
class Breaker : public Source, public Sink {
 public:
  ~Breaker() override;

  void Reset() final;

  // Fills on the first call, then hands over what it made, a chunk at a time.
  RowBatch* Next() final;

  base::Status status() const final;

 protected:
  explicit Breaker(Source& source) : source_(source) {}

  // The next chunk of the answer, or null once there are none left.
  virtual RowBatch* Emit() = 0;

  // Drops whatever was built, so the whole thing can be run again.
  virtual void Rewind() = 0;

 private:
  base::Status Fill();

  Source& source_;
  bool filled_ = false;
  base::Status status_ = base::OkStatus();
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_BREAKER_H_
