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

#ifndef SRC_TRACE_PROCESSOR_SYSTRACE_TRACE_PARSER_H_
#define SRC_TRACE_PROCESSOR_SYSTRACE_TRACE_PARSER_H_

#include <deque>
#include <regex>

#include "src/trace_processor/chunked_trace_reader.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class SystraceTraceParser : public ChunkedTraceReader {
 public:
  explicit SystraceTraceParser(TraceProcessorContext*);
  ~SystraceTraceParser() override;

  // ChunkedTraceReader implementation.
  util::Status Parse(std::unique_ptr<uint8_t[]>, size_t size) override;

 private:
  enum ParseState {
    kBeforeParse,
    kHtmlBeforeSystrace,
    kSystrace,
    kEndOfSystrace,
  };

  util::Status ParseSingleSystraceEvent(const std::string& buffer);

  TraceProcessorContext* const context_;
  const StringId sched_wakeup_name_id_ = 0;
  const StringId cpu_idle_name_id_ = 0;

  ParseState state_ = ParseState::kBeforeParse;

  // Used to glue together trace packets that span across two (or more)
  // Parse() boundaries.
  std::deque<uint8_t> partial_buf_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SYSTRACE_TRACE_PARSER_H_
