/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PROTO_TRACE_PARSER_H_
#define SRC_TRACE_PROCESSOR_PROTO_TRACE_PARSER_H_

#include <stdint.h>

#include <array>
#include <memory>

#include "perfetto/base/string_view.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

struct SystraceTracePoint {
  char phase;
  uint32_t tid;

  // For phase = 'B' and phase = 'C' only.
  base::StringView name;

  // For phase = 'C' only.
  double value;
};

inline bool operator==(const SystraceTracePoint& x,
                       const SystraceTracePoint& y) {
  return std::tie(x.phase, x.tid, x.name, x.value) ==
         std::tie(y.phase, y.tid, y.name, y.value);
}

bool ParseSystraceTracePoint(base::StringView, SystraceTracePoint* out);

class ProtoTraceParser {
 public:
  explicit ProtoTraceParser(TraceProcessorContext*);
  virtual ~ProtoTraceParser();

  // virtual for testing.
  virtual void ParseTracePacket(uint64_t timestamp, TraceBlobView);
  virtual void ParseFtracePacket(uint32_t cpu,
                                 uint64_t timestamp,
                                 TraceBlobView);
  void ParseProcessTree(TraceBlobView);
  void ParseProcessStats(uint64_t timestamp, TraceBlobView);
  void ParseProcMemCounters(uint64_t timestamp, TraceBlobView);
  void ParseSchedSwitch(uint32_t cpu, uint64_t timestamp, TraceBlobView);
  void ParseCpuFreq(uint64_t timestamp, TraceBlobView);
  void ParseCpuIdle(uint64_t timestamp, TraceBlobView);
  void ParsePrint(uint32_t cpu,
                  uint64_t timestamp,
                  uint32_t pid,
                  TraceBlobView);
  void ParseThread(TraceBlobView);
  void ParseProcess(TraceBlobView);
  void ParseSysStats(uint64_t ts, TraceBlobView);
  void ParseMemInfo(uint64_t ts, TraceBlobView);
  void ParseVmStat(uint64_t ts, TraceBlobView);
  void ParseCpuTimes(uint64_t ts, TraceBlobView);
  void ParseIrqCount(uint64_t ts, TraceBlobView, bool is_soft);
  void ParseRssStat(uint64_t ts, uint32_t pid, TraceBlobView);
  void ParseIonHeapGrow(uint64_t ts, uint32_t pid, TraceBlobView);
  void ParseIonHeapShrink(uint64_t ts, uint32_t pid, TraceBlobView);
  void ParseSignalDeliver(uint64_t ts, uint32_t pid, TraceBlobView);
  void ParseSignalGenerate(uint64_t ts, TraceBlobView);
  void ParseLowmemoryKill(uint64_t ts, TraceBlobView);
  void ParseBatteryCounters(uint64_t ts, TraceBlobView);

 private:
  TraceProcessorContext* context_;
  const StringId utid_name_id_;
  const StringId cpu_freq_name_id_;
  const StringId cpu_idle_name_id_;
  const StringId num_forks_name_id_;
  const StringId num_irq_total_name_id_;
  const StringId num_softirq_total_name_id_;
  const StringId num_irq_name_id_;
  const StringId num_softirq_name_id_;
  const StringId cpu_times_user_ns_id_;
  const StringId cpu_times_user_ice_ns_id_;
  const StringId cpu_times_system_mode_ns_id_;
  const StringId cpu_times_idle_ns_id_;
  const StringId cpu_times_io_wait_ns_id_;
  const StringId cpu_times_irq_ns_id_;
  const StringId cpu_times_softirq_ns_id_;
  const StringId ion_heap_grow_id_;
  const StringId ion_heap_shrink_id_;
  const StringId signal_deliver_id_;
  const StringId signal_generate_id_;
  const StringId batt_charge_id_;
  const StringId batt_capacity_id_;
  const StringId batt_current_id_;
  const StringId batt_current_avg_id_;
  std::vector<StringId> meminfo_strs_id_;
  std::vector<StringId> vmstat_strs_id_;
  std::vector<StringId> rss_members_;

  // Maps a proto field number from ProcessStats::MemCounters to its StringId.
  // Keep kProcMemCounterSize equal to 1 + max proto field id of MemCounters.
  static constexpr size_t kProcMemCounterSize = 10;
  std::array<StringId, kProcMemCounterSize> proc_mem_counter_names_{};
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PROTO_TRACE_PARSER_H_
