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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_STRACE_STRACE_TRACE_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_STRACE_STRACE_TRACE_TOKENIZER_H_

#include <cstdint>
#include <memory>
#include <optional>

#include "perfetto/base/status.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/plugins/strace/strace_event.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto::trace_processor::strace_importer {

class StraceTraceTokenizer : public ChunkedTraceReader {
 public:
  explicit StraceTraceTokenizer(TraceProcessorContext*);
  ~StraceTraceTokenizer() override;

  base::Status Parse(TraceBlobView) override;
  base::Status OnPushDataToSorter() override { return base::OkStatus(); }
  void OnEventsFullyExtracted() override {}

 private:
  TraceProcessorContext* const context_;
  util::TraceBlobViewReader reader_;
  std::unique_ptr<TraceSorter::Stream<StraceEvent>> stream_;
};

}  // namespace perfetto::trace_processor::strace_importer

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_STRACE_STRACE_TRACE_TOKENIZER_H_
