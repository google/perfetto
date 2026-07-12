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

#include "src/trace_processor/shell/report/report_view.h"

#include <cstdint>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "protos/perfetto/trace_processor/report.pbzero.h"
#include "src/trace_processor/shell/report/report_sink.h"
#include "src/trace_processor/shell/report/view_common.h"

namespace perfetto::trace_processor::shell {

ReportView::~ReportView() = default;

base::Status EmitHeader(TraceProcessor* tp,
                        const std::string& trace_file,
                        ReportSink* sink) {
  int64_t start = 0;
  int64_t end = 0;
  {
    auto it = tp->ExecuteQuery("SELECT start_ts, end_ts FROM trace_bounds");
    if (it.Next()) {
      start = AsI64(it.Get(0));
      end = AsI64(it.Get(1));
    }
    RETURN_IF_ERROR(it.Status());
  }
  int64_t process_count = 0;
  {
    auto it = tp->ExecuteQuery("SELECT count() FROM process");
    if (it.Next())
      process_count = AsI64(it.Get(0));
    RETURN_IF_ERROR(it.Status());
  }

  protozero::HeapBuffered<protos::pbzero::ReportPacket> packet;
  auto* h = packet->set_header();
  if (!trace_file.empty())
    h->set_trace_file(trace_file);
  h->set_trace_start_ns(start);
  h->set_trace_end_ns(end);
  h->set_trace_dur_ns(end - start);
  h->set_process_count(process_count);
  return EmitPacket(sink, &packet);
}

}  // namespace perfetto::trace_processor::shell
