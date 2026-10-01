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

#ifndef INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PARSER_H_
#define INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PARSER_H_

#include <memory>
#include "perfetto/base/export.h"
#include "perfetto/base/status.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "perfetto/trace_processor/trace_parser_config.h"
#include "perfetto/trace_processor/trace_parser_sinks.h"

namespace perfetto::trace_processor {

// Decodes and normalizes traces without creating a query engine. Parser working
// tables remain internal; output receives records from the first startup
// insert. This prototype supports protobuf (including TrackEvent and ftrace)
// and JSON.
class PERFETTO_EXPORT_COMPONENT TraceParser {
 public:
  using Config = TraceParserConfig;
  using Sinks = TraceParserSinks;
  static std::unique_ptr<TraceParser> CreateInstance(const Config&, Sinks = {});
  virtual ~TraceParser();

  // Push a chunk of trace bytes. Errors are sticky until destruction.
  virtual base::Status Parse(TraceBlobView) = 0;

  // Drain input, emit end-of-trace patches and release parser working state.
  // Further Parse/NotifyEndOfFile calls return an error.
  virtual base::Status NotifyEndOfFile() = 0;
};
}  // namespace perfetto::trace_processor
#endif  // INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PARSER_H_
