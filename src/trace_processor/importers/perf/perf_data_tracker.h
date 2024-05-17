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

#include "perfetto/ext/base/status_or.h"
#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"
#include "src/trace_processor/importers/perf/perf_event.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {
namespace perf_importer {

class Reader;
using MappingTable = tables::StackProfileMappingTable;

class PerfDataTracker : public Destructible {
 public:
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
    protos::pbzero::Profiling::CpuMode cpu_mode;
    Numeric num;
    std::string filename;
  };

  PerfDataTracker(const PerfDataTracker&) = delete;
  PerfDataTracker& operator=(const PerfDataTracker&) = delete;
  explicit PerfDataTracker(TraceProcessorContext* context)
      : context_(context) {}
  ~PerfDataTracker() override;
  static PerfDataTracker* GetOrCreate(TraceProcessorContext* context);

  void PushMmap2Record(Mmap2Record record);

  base::StatusOr<PerfSample> ParseSample(Reader&, uint64_t sample_type);

 private:
  TraceProcessorContext* context_;
};
}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_TRACKER_H_
