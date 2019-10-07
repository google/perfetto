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

#include "tools/trace_to_text/trace_to_json.h"

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "tools/trace_to_text/utils.h"

namespace perfetto {
namespace trace_to_text {

namespace {

const char kTraceHeader[] = R"({
  "traceEvents": [
)";

const char kTraceFooter[] = R"(\n",
  "controllerTraceDataKey": "systraceController"
})";

}  // namespace

int TraceToJson(std::istream* input,
                std::ostream* output,
                bool compress,
                Keep truncate_keep) {
  std::unique_ptr<TraceWriter> trace_writer(
      compress ? new DeflateTraceWriter(output) : new TraceWriter(output));

  trace_processor::Config config;
  std::unique_ptr<trace_processor::TraceProcessor> tp =
      trace_processor::TraceProcessor::CreateInstance(config);

  if (!ReadTrace(tp.get(), input))
    return 1;
  tp->NotifyEndOfFile();

  trace_writer->Write(kTraceHeader);

  // TODO(eseckler): support userspace event conversion.
  fprintf(stderr, "Converting userspace events%c", kProgressChar);
  fflush(stderr);

  trace_writer->Write("],\n");

  int ret = ExtractSystrace(tp.get(), trace_writer.get(),
                            /*wrapped_in_json=*/true, truncate_keep);
  if (ret)
    return ret;

  trace_writer->Write(kTraceFooter);
  return 0;
}

}  // namespace trace_to_text
}  // namespace perfetto
