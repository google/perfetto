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

namespace perfetto::trace_processor::core::exec {

class RowBatch;

// Somewhere a stream of chunks can be poured.
//
// Chunks are pushed into it. The opposite end of a Source, which has chunks
// pulled out of it, and the half of a Breaker that faces its input.
//
// The two are separate so that being fed and being read are things a step can
// do rather than things it is: a Breaker does both.
class Sink {
 public:
  virtual ~Sink();

  // Called with every chunk of the input, in order. The chunk belongs to
  // whoever produced it and is refilled on the next pull, so anything worth
  // keeping has to be kept here.
  virtual base::Status Consume(RowBatch&) = 0;

  // Called once, after the last chunk: everything is now known.
  virtual base::Status Finish() = 0;

 protected:
  Sink() = default;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_SINK_H_
