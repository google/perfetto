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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERFETTO_METADATA_PERFETTO_METADATA_READER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERFETTO_METADATA_PERFETTO_METADATA_READER_H_

#include <optional>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Reads a perfetto_metadata sidecar file: a JSON file inside an archive
// (zip/tar) which overrides clock and machine handling for the other files
// in the archive. The parsed configuration is stored on the global
// TraceMetadataState and consulted by ForwardingTraceParser when each other
// archive member is initialized.
class PerfettoMetadataReader : public ChunkedTraceReader {
 public:
  explicit PerfettoMetadataReader(TraceProcessorContext* context);
  ~PerfettoMetadataReader() override;

  // ChunkedTraceReader implementation.
  base::Status Parse(TraceBlobView) override;
  base::Status OnPushDataToSorter() override;
  void OnEventsFullyExtracted() override {}

  // Parses a builtin clock name ("BOOTTIME", "REALTIME", ...) into its
  // protos::pbzero::BuiltinClock value.
  static std::optional<uint32_t> ParseClockName(const std::string& name);

 private:
  TraceProcessorContext* const context_;
  std::string buffer_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERFETTO_METADATA_PERFETTO_METADATA_READER_H_
