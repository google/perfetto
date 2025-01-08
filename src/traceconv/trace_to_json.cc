/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/traceconv/trace_to_json.h"

#include <stdio.h>
#include <istream>
#include <memory>
#include <ostream>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/trace_processor/export_json.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/traceconv/trace_to_systrace.h"
#include "src/traceconv/utils.h"

namespace perfetto::trace_to_text {

namespace {

const char kTraceHeader[] = R"({
  "traceEvents": [],
)";

const char kTraceFooter[] = R"(,
  "controllerTraceDataKey": "systraceController"
})";

bool ExportUserspaceEvents(trace_processor::TraceProcessor* tp,
                           TraceWriter* writer) {
  fprintf(stderr, "Converting userspace events%c", kProgressChar);
  fflush(stderr);

  struct StringWriter : public trace_processor::json::OutputWriter {
    base::Status AppendString(const std::string& s) override {
      res.append(s);
      return base::OkStatus();
    }
    std::string res;
  };

  StringWriter string_writer;
  base::Status status = trace_processor::json::ExportJson(tp, &string_writer);
  if (!status.ok()) {
    PERFETTO_ELOG("Could not convert userspace events: %s", status.c_message());
    return false;
  }

  // Skip writing the closing brace since we'll append system trace data.
  writer->Write(string_writer.res.data(), string_writer.res.size() - 1);

  return true;
}

}  // namespace

int TraceToJson(std::istream* input,
                std::ostream* output,
                bool compress,
                Keep truncate_keep,
                bool full_sort) {
  std::unique_ptr<TraceWriter> trace_writer(
      compress ? new DeflateTraceWriter(output) : new TraceWriter(output));

  trace_processor::Config config;
  config.sorting_mode = full_sort
                            ? trace_processor::SortingMode::kForceFullSort
                            : trace_processor::SortingMode::kDefaultHeuristics;
  std::unique_ptr<trace_processor::TraceProcessor> tp =
      trace_processor::TraceProcessor::CreateInstance(config);

  if (!ReadTraceUnfinalized(tp.get(), input))
    return 1;
  if (auto status = tp->NotifyEndOfFile(); !status.ok()) {
    return 1;
  }

  // TODO(eseckler): Support truncation of userspace event data.
  if (ExportUserspaceEvents(tp.get(), trace_writer.get())) {
    trace_writer->Write(",\n");
  } else {
    trace_writer->Write(kTraceHeader);
  }

  int ret = ExtractSystrace(tp.get(), trace_writer.get(),
                            /*wrapped_in_json=*/true, truncate_keep);
  if (ret)
    return ret;

  trace_writer->Write(kTraceFooter);
  return 0;
}

}  // namespace perfetto::trace_to_text
