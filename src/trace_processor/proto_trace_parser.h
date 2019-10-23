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

#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/timestamped_trace_piece.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_parser.h"
#include "src/trace_processor/trace_storage.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

class ArgsTracker;
class PacketSequenceState;
class TraceProcessorContext;

class ProtoTraceParser : public TraceParser {
 public:
  using ConstBytes = protozero::ConstBytes;
  explicit ProtoTraceParser(TraceProcessorContext*);
  ~ProtoTraceParser() override;

  // TraceParser implementation.
  void ParseTracePacket(int64_t timestamp, TimestampedTracePiece) override;
  void ParseFtracePacket(uint32_t cpu,
                         int64_t timestamp,
                         TimestampedTracePiece) override;

  void ParseTracePacketImpl(int64_t ts,
                            TimestampedTracePiece,
                            const protos::pbzero::TracePacket::Decoder&);

  void ParseProcessTree(ConstBytes);
  void ParseProcessStats(int64_t timestamp, ConstBytes);
  void ParseSysStats(int64_t ts, ConstBytes);
  void ParseBatteryCounters(int64_t ts, ConstBytes);
  void ParsePowerRails(int64_t ts, ConstBytes);
  void ParseAndroidLogPacket(ConstBytes);
  void ParseAndroidLogEvent(ConstBytes);
  void ParseAndroidLogStats(ConstBytes);
  void ParseTraceStats(ConstBytes);
  void ParseProfilePacket(int64_t ts,
                          PacketSequenceState*,
                          size_t sequence_state_generation,
                          ConstBytes);
  void ParseStreamingProfilePacket(PacketSequenceState*,
                                   size_t sequence_state_generation,
                                   ConstBytes);
  void ParseSystemInfo(ConstBytes);
  void ParseChromeBenchmarkMetadata(ConstBytes);
  void ParseChromeEvents(int64_t ts, ConstBytes);
  void ParseMetatraceEvent(int64_t ts, ConstBytes);
  void ParseTraceConfig(ConstBytes);
  void ParseStatsdMetadata(ConstBytes);
  void ParseAndroidPackagesList(ConstBytes);
  void ParseModuleSymbols(ConstBytes);
  void ParseHeapGraph(int64_t ts, ConstBytes);

 private:
  TraceProcessorContext* context_;

  const StringId utid_name_id_;
  const StringId num_forks_name_id_;
  const StringId num_irq_total_name_id_;
  const StringId num_softirq_total_name_id_;
  const StringId num_irq_name_id_;
  const StringId num_softirq_name_id_;
  const StringId cpu_times_user_ns_id_;
  const StringId cpu_times_user_nice_ns_id_;
  const StringId cpu_times_system_mode_ns_id_;
  const StringId cpu_times_idle_ns_id_;
  const StringId cpu_times_io_wait_ns_id_;
  const StringId cpu_times_irq_ns_id_;
  const StringId cpu_times_softirq_ns_id_;
  const StringId batt_charge_id_;
  const StringId batt_capacity_id_;
  const StringId batt_current_id_;
  const StringId batt_current_avg_id_;
  const StringId oom_score_adj_id_;
  const StringId metatrace_id_;
  const StringId data_name_id_;
  const StringId raw_chrome_metadata_event_id_;
  const StringId raw_chrome_legacy_system_trace_event_id_;
  const StringId raw_chrome_legacy_user_trace_event_id_;
  std::vector<StringId> meminfo_strs_id_;
  std::vector<StringId> vmstat_strs_id_;
  std::vector<StringId> power_rails_strs_id_;

  // Maps a proto field number for memcounters in ProcessStats::Process to
  // their StringId. Keep kProcStatsProcessSize equal to 1 + max proto field
  // id of ProcessStats::Process.
  static constexpr size_t kProcStatsProcessSize = 11;
  std::array<StringId, kProcStatsProcessSize> proc_stats_process_names_{};
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PROTO_TRACE_PARSER_H_
