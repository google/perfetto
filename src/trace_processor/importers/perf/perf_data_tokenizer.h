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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_TOKENIZER_H_

#include <stdint.h>
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/perf/perf_data_reader.h"
#include "src/trace_processor/importers/perf/perf_data_tracker.h"
#include "src/trace_processor/importers/perf/perf_event.h"

#include <limits>
#include <map>
#include <string>
#include <vector>

#include "src/trace_processor/importers/common/chunked_trace_reader.h"

namespace perfetto {
namespace trace_processor {
namespace perf_importer {

using Section = PerfDataTracker::PerfFileSection;

class PerfDataTokenizer : public ChunkedTraceReader {
 public:
  struct PerfHeader {
    static constexpr char PERF_MAGIC[] = "PERFILE2";

    char magic[8];
    uint64_t size;
    // Size of PerfFileAttr struct and section pointing to ids.
    uint64_t attr_size;
    Section attrs;
    Section data;
    Section event_types;
    uint64_t flags;
    uint64_t flags1[3];

    uint64_t num_attrs() const { return attrs.size / attr_size; }
  };

  explicit PerfDataTokenizer(TraceProcessorContext*);
  ~PerfDataTokenizer() override;
  PerfDataTokenizer(const PerfDataTokenizer&) = delete;
  PerfDataTokenizer& operator=(const PerfDataTokenizer&) = delete;

  // ChunkedTraceReader implementation
  base::Status Parse(TraceBlobView) override;
  void NotifyEndOfFile() override;

 private:
  enum class ParsingState {
    Header = 0,
    AfterHeaderBuffer = 1,
    Attrs = 2,
    AttrIds = 3,
    AttrIdsFromBuffer = 4,
    Records = 5
  };
  enum class ParsingResult { NoSpace = 0, Success = 1 };

  base::StatusOr<ParsingResult> ParseHeader();
  base::StatusOr<ParsingResult> ParseAfterHeaderBuffer();
  base::StatusOr<ParsingResult> ParseAttrs();
  base::StatusOr<ParsingResult> ParseAttrIds();
  base::StatusOr<ParsingResult> ParseAttrIdsFromBuffer();

  base::StatusOr<PerfDataTracker::Mmap2Record> ParseMmap2Record(
      uint64_t record_size);

  bool ValidateSample(const PerfDataTracker::PerfSample&);

  TraceProcessorContext* context_;
  PerfDataTracker* tracker_;

  ParsingState parsing_state_ = ParsingState::Header;

  PerfHeader header_;

  std::vector<PerfDataTracker::PerfFileAttr> attrs_;
  uint64_t ids_start_ = std::numeric_limits<uint64_t>::max();
  uint64_t ids_end_ = 0;
  std::vector<uint8_t> after_header_buffer_;

  perf_importer::Reader reader_;
};

}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_TOKENIZER_H_
