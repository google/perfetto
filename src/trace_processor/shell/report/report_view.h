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

#ifndef SRC_TRACE_PROCESSOR_SHELL_REPORT_REPORT_VIEW_H_
#define SRC_TRACE_PROCESSOR_SHELL_REPORT_REPORT_VIEW_H_

#include <string>

#include "perfetto/base/status.h"

namespace perfetto::trace_processor {
class TraceProcessor;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::shell {

class ReportSink;
struct Scope;

// A single opinionated view over one trace dimension (a noun/view pair). Every
// view implements the same contract, so cross-cutting features -- running,
// --format sql, and anything added later -- apply to all views uniformly. Add a
// view by subclassing this and registering it in report_subcommand.cc; nothing
// else needs to special-case it.
class ReportView {
 public:
  virtual ~ReportView();

  // The SQL that produces this view's rows. Surfaced verbatim by --format sql
  // and used by Emit(), so the two never drift.
  virtual std::string Sql(const Scope& scope) const = 0;

  // Runs the view against |tp| and emits its packets (a SectionInfo followed by
  // the row packets) to |sink|. If |omit_if_empty| the whole section is skipped
  // when the trace has no matching data in scope (used by the overview).
  virtual base::Status Emit(TraceProcessor* tp,
                            const Scope& scope,
                            ReportSink* sink,
                            bool omit_if_empty) const = 0;
};

// Emits the trace-level ReportHeader packet (duration, process count) that opens
// every report stream. Not a view: it is the stream preamble.
base::Status EmitHeader(TraceProcessor* tp,
                        const std::string& trace_file,
                        ReportSink* sink);

}  // namespace perfetto::trace_processor::shell

#endif  // SRC_TRACE_PROCESSOR_SHELL_REPORT_REPORT_VIEW_H_
