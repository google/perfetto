/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_BIGTRACE_TRACE_PROCESSOR_WRAPPER_H_
#define SRC_BIGTRACE_TRACE_PROCESSOR_WRAPPER_H_

#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/base/threading/stream.h"
#include "perfetto/ext/base/threading/thread_pool.h"
#include "perfetto/ext/trace_processor/rpc/query_result_serializer.h"
#include "perfetto/trace_processor/trace_processor.h"

namespace perfetto {
namespace protos {

class QueryTraceResponse;

}  // namespace protos
}  // namespace perfetto

namespace perfetto {
namespace bigtrace {

// Wrapper class around an instance of TraceProcessor to adapt it for the needs
// of a BigTrace Worker.
class TraceProcessorWrapper {
 public:
  enum Statefulness {
    // Indicates that the state of the trace processor instance should be purged
    // after every query.
    kStateless,

    // Indicates that the state of the trace processor instance should be
    // preserved across queries.
    kStateful,
  };

  TraceProcessorWrapper(std::string trace_path,
                        base::ThreadPool*,
                        Statefulness);

  // Loads the trace given a stream of chunks to parse.
  base::StatusFuture LoadTrace(
      base::StatusOrStream<std::vector<uint8_t>> file_stream);

  // Executes the given query on the trace processor and returns the results
  // as a stream.
  base::StatusOrStream<protos::QueryTraceResponse> Query(
      const std::string& sql);

 private:
  using TraceProcessor = trace_processor::TraceProcessor;

  TraceProcessorWrapper(const TraceProcessorWrapper&) = delete;
  TraceProcessorWrapper& operator=(const TraceProcessorWrapper&) = delete;

  TraceProcessorWrapper(TraceProcessorWrapper&&) = delete;
  TraceProcessorWrapper& operator=(TraceProcessorWrapper&&) = delete;

  const std::string trace_path_;
  base::ThreadPool* thread_pool_ = nullptr;
  const Statefulness statefulness_ = Statefulness::kStateless;
  std::shared_ptr<TraceProcessor> trace_processor_;
};

}  // namespace bigtrace
}  // namespace perfetto

#endif  // SRC_BIGTRACE_TRACE_PROCESSOR_WRAPPER_H_
