/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_IMPL_H_
#define SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_IMPL_H_

#include <atomic>
#include <functional>
#include <memory>

#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/scoped_db.h"
#include "src/trace_processor/trace_processor_context.h"

namespace perfetto {

namespace protos {
class RawQueryArgs;
class RawQueryResult;
}  // namespace protos

namespace trace_processor {

// Coordinates the loading of traces from an arbitrary source and allows
// execution of SQL queries on the events in these traces.
class TraceProcessorImpl : public TraceProcessor {
 public:
  explicit TraceProcessorImpl(const Config&);

  ~TraceProcessorImpl() override;

  bool Parse(std::unique_ptr<uint8_t[]>, size_t) override;

  void NotifyEndOfFile() override;

  void ExecuteQuery(
      const protos::RawQueryArgs&,
      std::function<void(const protos::RawQueryResult&)>) override;

  void InterruptQuery() override;

 private:
  ScopedDb db_;  // Keep first.
  TraceProcessorContext context_;
  bool unrecoverable_parse_error_ = false;

  // This is atomic because it is set by the CTRL-C signal handler and we need
  // to prevent single-flow compiler optimizations in ExecuteQuery().
  std::atomic<bool> query_interrupted_{false};
};
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_IMPL_H_
