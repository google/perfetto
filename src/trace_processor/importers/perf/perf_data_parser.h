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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_PARSER_H_

#include <stdint.h>

#include "perfetto/base/compiler.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/importers/perf/perf_data_tracker.h"

namespace perfetto {
namespace trace_processor {
namespace perf_importer {

// Parses samples from perf.data files.
class PerfDataParser : public TraceParser {
 public:
  explicit PerfDataParser(TraceProcessorContext*);
  ~PerfDataParser() override;

  // The data in TraceBlobView has to be a perf.data sample.
  void ParseTraceBlobView(int64_t timestamp, TraceBlobView) override;

 private:
  base::StatusOr<PerfDataTracker::PerfSample> ParseSample(TraceBlobView);

  TraceProcessorContext* context_ = nullptr;
  PerfDataTracker* tracker_ = nullptr;
};

}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_PARSER_H_
