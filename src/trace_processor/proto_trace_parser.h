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
  uint32_t tgid;

  // For phase = 'B' and phase = 'C' only.
  base::StringView name;

  // For phase = 'C' only.
  double value;
};

inline bool operator==(const SystraceTracePoint& x,
                       const SystraceTracePoint& y) {
  return std::tie(x.phase, x.tgid, x.name, x.value) ==
         std::tie(y.phase, y.tgid, y.name, y.value);
}

bool ParseSystraceTracePoint(base::StringView, SystraceTracePoint* out);

class ProtoTraceParser {
 public:
  explicit ProtoTraceParser(TraceProcessorContext*);
  virtual ~ProtoTraceParser();

  // virtual for testing.
  virtual void ParseTracePacket(int64_t timestamp, TraceBlobView);
  virtual void ParseFtracePacket(uint32_t cpu,
                                 int64_t timestamp,
                                 TraceBlobView);
  void ParseProcessTree(TraceBlobView);
  void ParseProcessStats(int64_t timestamp, TraceBlobView);
  void ParseProcessStatsProcess(int64_t timestamp, TraceBlobView);
  void ParseSchedSwitch(uint32_t cpu, int64_t timestamp, TraceBlobView);
  void ParseCpuFreq(int64_t timestamp, TraceBlobView);
  void ParseCpuIdle(int64_t timestamp, TraceBlobView);
  void ParsePrint(uint32_t cpu, int64_t timestamp, uint32_t pid, TraceBlobView);
  void ParseThread(TraceBlobView);
  void ParseProcess(TraceBlobView);
  void ParseSysStats(int64_t ts, TraceBlobView);
  void ParseMemInfo(int64_t ts, TraceBlobView);
  void ParseVmStat(int64_t ts, TraceBlobView);
  void ParseCpuTimes(int64_t ts, TraceBlobView);
  void ParseIrqCount(int64_t ts, TraceBlobView, bool is_soft);
  void ParseRssStat(int64_t ts, uint32_t pid, TraceBlobView);
  void ParseIonHeapGrowOrShrink(int64_t ts,
                                uint32_t pid,
                                TraceBlobView,
                                bool grow);
  void ParseSignalDeliver(int64_t ts, uint32_t pid, TraceBlobView);
  void ParseSignalGenerate(int64_t ts, TraceBlobView);
  void ParseLowmemoryKill(int64_t ts, TraceBlobView);
  void ParseBatteryCounters(int64_t ts, TraceBlobView);
  void ParseOOMScoreAdjUpdate(int64_t ts, TraceBlobView);
  void ParseMmEventRecordField(int64_t ts, uint32_t pid, TraceBlobView);
  void ParseClockSnapshot(TraceBlobView);
  std::pair<int /*type*/, int64_t> ParseClockField(TraceBlobView);
  void ParseAndroidLogPacket(TraceBlobView);
  void ParseAndroidLogEvent(TraceBlobView);
  void ParseAndroidLogBinaryArg(TraceBlobView, char** str, size_t avail);
  void ParseAndroidLogStats(TraceBlobView);
  void ParseGenericFtrace(int64_t timestamp,
                          uint32_t cpu,
                          uint32_t pid,
                          TraceBlobView view);
  void ParseGenericFtraceField(RowId generic_row_id, TraceBlobView view);
  void ParseTypedFtraceToRaw(uint32_t ftrace_id,
                             int64_t timestamp,
                             uint32_t cpu,
                             uint32_t pid,
                             TraceBlobView view);
  void ParseTraceStats(TraceBlobView);
  void ParseFtraceStats(TraceBlobView);
  void ParseProfilePacket(TraceBlobView);

 private:
  TraceProcessorContext* context_;
  const StringId utid_name_id_;
  const StringId cpu_freq_name_id_;
  const StringId cpu_idle_name_id_;
  const StringId comm_name_id_;
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
  const StringId signal_deliver_id_;
  const StringId signal_generate_id_;
  const StringId batt_charge_id_;
  const StringId batt_capacity_id_;
  const StringId batt_current_id_;
  const StringId batt_current_avg_id_;
  const StringId lmk_id_;
  const StringId oom_score_adj_id_;
  const StringId ion_total_unknown_id_;
  const StringId ion_change_unknown_id_;
  std::vector<StringId> meminfo_strs_id_;
  std::vector<StringId> vmstat_strs_id_;
  std::vector<StringId> rss_members_;

  // Maps a proto field number for memcounters in ProcessStats::Process to their
  // StringId. Keep kProcStatsProcessSize equal to 1 + max proto field id of
  // ProcessStats::process.
  static constexpr size_t kProcStatsProcessSize = 11;
  std::array<StringId, kProcStatsProcessSize> proc_stats_process_names_{};

  struct MmEventCounterNames {
    MmEventCounterNames() = default;
    MmEventCounterNames(StringId _count, StringId _max_lat, StringId _avg_lat)
        : count(_count), max_lat(_max_lat), avg_lat(_avg_lat) {}

    StringId count = 0;
    StringId max_lat = 0;
    StringId avg_lat = 0;
  };

  // Keep kMmEventCounterSize equal to mm_event_type::MM_TYPE_NUM in the kernel.
  static constexpr size_t kMmEventCounterSize = 7;
  std::array<MmEventCounterNames, kMmEventCounterSize> mm_event_counter_names_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PROTO_TRACE_PARSER_H_
