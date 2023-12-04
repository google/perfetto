/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_TRACKER_H_

#include <cstdint>
#include <string>
#include <vector>
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/perf/perf_data_reader.h"
#include "src/trace_processor/importers/perf/perf_event.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {
namespace perf_importer {

using MappingTable = tables::StackProfileMappingTable;

class PerfDataTracker : public Destructible {
 public:
  struct PerfFileSection {
    uint64_t offset;
    uint64_t size;

    uint64_t end() const { return offset + size; }
  };
  struct PerfFileAttr {
    perf_event_attr attr;
    PerfFileSection ids;
  };
  struct AttrAndIds {
    perf_event_attr attr;
    std::vector<uint64_t> ids;
  };
  struct PerfSample {
    std::optional<uint64_t> id = 0;
    std::optional<uint32_t> pid = 0;
    std::optional<uint32_t> tid = 0;
    std::optional<uint64_t> ts = 0;
    std::optional<uint32_t> cpu = 0;
    std::vector<uint64_t> callchain;
  };
  struct Mmap2Record {
    struct Numeric {
      uint32_t pid;
      uint32_t tid;
      uint64_t addr;
      uint64_t len;
      uint64_t pgoff;
      uint32_t maj;
      uint32_t min;
      uint64_t ino;
      uint64_t ino_generation;
      uint32_t prot;
      uint32_t flags;
    };
    Numeric num;
    std::string filename;
  };
  struct MmapRange {
    uint64_t start;
    uint64_t end;
    MappingTable::Id id;
  };

  PerfDataTracker(const PerfDataTracker&) = delete;
  PerfDataTracker& operator=(const PerfDataTracker&) = delete;
  explicit PerfDataTracker(TraceProcessorContext* context)
      : context_(context) {}
  ~PerfDataTracker() override;
  static PerfDataTracker* GetOrCreate(TraceProcessorContext* context);

  uint64_t ComputeCommonSampleType();

  void PushAttrAndIds(AttrAndIds data) { attrs_.push_back(std::move(data)); }

  void PushMmap2Record(Mmap2Record record);

  uint64_t common_sample_type() { return common_sample_type_; }

  base::StatusOr<PerfSample> ParseSample(
      perfetto::trace_processor::perf_importer::Reader&);

  base::StatusOr<MmapRange> FindMapping(uint32_t pid, uint64_t ips);

 private:
  const perf_event_attr* FindAttrWithId(uint64_t id) const;
  TraceProcessorContext* context_;
  std::vector<AttrAndIds> attrs_;

  base::FlatHashMap</*pid=*/uint32_t, std::vector<MmapRange>> mmap2_ranges_;
  uint64_t common_sample_type_;
};
}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_TRACKER_H_
