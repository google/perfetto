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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERF_RECORD_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERF_RECORD_PARSER_H_

#include <stdint.h>
#include <cstdint>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/importers/perf/mmap_record.h"
#include "src/trace_processor/importers/perf/record.h"
#include "src/trace_processor/importers/perf/sample.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

namespace perf_importer {

class PerfDataTracker;
class Reader;

// Parses samples from perf.data files.
class RecordParser : public PerfRecordParser {
 public:
  explicit RecordParser(TraceProcessorContext*);
  ~RecordParser() override;

  void ParsePerfRecord(int64_t timestamp, Record record) override;

 private:
  base::Status ParseRecord(int64_t timestamp, Record record);
  base::Status ParseSample(int64_t ts, Record record);
  base::Status ParseComm(Record record);
  base::Status ParseMmap(Record record);
  base::Status ParseMmap2(Record record);

  base::Status InternSample(Sample sample);

  base::Status UpdateCounters(const Sample& sample);
  base::Status UpdateCountersInReadGroups(const Sample& sample);

  std::optional<CallsiteId> InternCallchain(
      UniquePid upid,
      const std::vector<Sample::Frame>& callchain);

  UniquePid GetUpid(const CommonMmapRecordFields& fields) const;

  TraceProcessorContext* context_ = nullptr;
};

}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_RECORD_PARSER_H_
