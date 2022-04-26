/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_FTRACE_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_FTRACE_PARSER_H_

#include "perfetto/trace_processor/status.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/ftrace/drm_tracker.h"
#include "src/trace_processor/importers/ftrace/ftrace_descriptors.h"
#include "src/trace_processor/importers/ftrace/rss_stat_tracker.h"
#include "src/trace_processor/importers/ftrace/sched_event_tracker.h"
#include "src/trace_processor/timestamped_trace_piece.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

class FtraceParser {
 public:
  explicit FtraceParser(TraceProcessorContext* context);

  void ParseFtraceStats(protozero::ConstBytes);

  util::Status ParseFtraceEvent(uint32_t cpu, const TimestampedTracePiece& ttp);

 private:
  void ParseGenericFtrace(int64_t timestamp,
                          uint32_t cpu,
                          uint32_t pid,
                          protozero::ConstBytes);
  void ParseTypedFtraceToRaw(uint32_t ftrace_id,
                             int64_t timestamp,
                             uint32_t cpu,
                             uint32_t pid,
                             protozero::ConstBytes,
                             PacketSequenceStateGeneration*);
  void ParseSchedSwitch(uint32_t cpu, int64_t timestamp, protozero::ConstBytes);
  void ParseSchedWakeup(int64_t timestamp, uint32_t pid, protozero::ConstBytes);
  void ParseSchedWaking(int64_t timestamp, uint32_t pid, protozero::ConstBytes);
  void ParseSchedProcessFree(int64_t timestamp, protozero::ConstBytes);
  void ParseCpuFreq(int64_t timestamp, protozero::ConstBytes);
  void ParseGpuFreq(int64_t timestamp, protozero::ConstBytes);
  void ParseCpuIdle(int64_t timestamp, protozero::ConstBytes);
  void ParsePrint(int64_t timestamp, uint32_t pid, protozero::ConstBytes);
  void ParseZero(int64_t timestamp, uint32_t pid, protozero::ConstBytes);
  void ParseSdeTracingMarkWrite(int64_t timestamp,
                                uint32_t pid,
                                protozero::ConstBytes);
  void ParseDpuTracingMarkWrite(int64_t timestamp,
                                uint32_t pid,
                                protozero::ConstBytes);
  void ParseG2dTracingMarkWrite(int64_t timestamp,
                                uint32_t pid,
                                protozero::ConstBytes);
  void ParseMaliTracingMarkWrite(int64_t timestamp,
                                 uint32_t pid,
                                 protozero::ConstBytes);
  void ParseIonHeapGrowOrShrink(int64_t timestamp,
                                uint32_t pid,
                                protozero::ConstBytes,
                                bool grow);
  void ParseIonStat(int64_t timestamp, uint32_t pid, protozero::ConstBytes);
  void ParseDmaHeapStat(int64_t timestamp, uint32_t pid, protozero::ConstBytes);
  void ParseSignalGenerate(int64_t timestamp, protozero::ConstBytes);
  void ParseSignalDeliver(int64_t timestamp,
                          uint32_t pid,
                          protozero::ConstBytes);
  void ParseLowmemoryKill(int64_t timestamp, protozero::ConstBytes);
  void ParseOOMScoreAdjUpdate(int64_t timestamp, protozero::ConstBytes);
  void ParseOOMKill(int64_t timestamp, protozero::ConstBytes);
  void ParseMmEventRecord(int64_t timestamp,
                          uint32_t pid,
                          protozero::ConstBytes);
  void ParseSysEvent(int64_t timestamp,
                     uint32_t pid,
                     bool is_enter,
                     protozero::ConstBytes);
  void ParseTaskNewTask(int64_t timestamp,
                        uint32_t source_tid,
                        protozero::ConstBytes);
  void ParseTaskRename(protozero::ConstBytes);
  void ParseBinderTransaction(int64_t timestamp,
                              uint32_t pid,
                              protozero::ConstBytes);
  void ParseBinderTransactionReceived(int64_t timestamp,
                                      uint32_t pid,
                                      protozero::ConstBytes);
  void ParseBinderTransactionAllocBuf(int64_t timestamp,
                                      uint32_t pid,
                                      protozero::ConstBytes);
  void ParseBinderLocked(int64_t timestamp,
                         uint32_t pid,
                         protozero::ConstBytes);
  void ParseBinderLock(int64_t timestamp, uint32_t pid, protozero::ConstBytes);
  void ParseBinderUnlock(int64_t timestamp,
                         uint32_t pid,
                         protozero::ConstBytes);
  void ParseClockSetRate(int64_t timestamp, protozero::ConstBytes);
  void ParseClockEnable(int64_t timestamp, protozero::ConstBytes);
  void ParseClockDisable(int64_t timestamp, protozero::ConstBytes);
  void ClockRate(int64_t timestamp,
                 base::StringView clock_name,
                 base::StringView subtitle,
                 uint64_t rate);
  void ParseScmCallStart(int64_t timestamp,
                         uint32_t pid,
                         protozero::ConstBytes);
  void ParseScmCallEnd(int64_t timestamp, uint32_t pid, protozero::ConstBytes);
  void ParseDirectReclaimBegin(int64_t timestamp,
                               uint32_t pid,
                               protozero::ConstBytes);
  void ParseDirectReclaimEnd(int64_t timestamp,
                             uint32_t pid,
                             protozero::ConstBytes);
  void ParseWorkqueueExecuteStart(uint32_t cpu,
                                  int64_t timestamp,
                                  uint32_t pid,
                                  protozero::ConstBytes,
                                  PacketSequenceStateGeneration* seq_state);
  void ParseWorkqueueExecuteEnd(int64_t timestamp,
                                uint32_t pid,
                                protozero::ConstBytes);
  void ParseIrqHandlerEntry(uint32_t cpu,
                            int64_t timestamp,
                            protozero::ConstBytes);
  void ParseIrqHandlerExit(uint32_t cpu,
                           int64_t timestamp,
                           protozero::ConstBytes);
  void ParseSoftIrqEntry(uint32_t cpu,
                         int64_t timestamp,
                         protozero::ConstBytes);
  void ParseSoftIrqExit(uint32_t cpu, int64_t timestamp, protozero::ConstBytes);
  void ParseGpuMemTotal(int64_t timestamp, protozero::ConstBytes);
  void ParseThermalTemperature(int64_t timestamp, protozero::ConstBytes);
  void ParseCdevUpdate(int64_t timestamp, protozero::ConstBytes);
  void ParseSchedBlockedReason(int64_t timestamp,
                               protozero::ConstBytes,
                               PacketSequenceStateGeneration*);
  void ParseFastRpcDmaStat(int64_t timestamp,
                           uint32_t pid,
                           protozero::ConstBytes);
  void ParseCpuhpPause(int64_t, uint32_t, protozero::ConstBytes);
  void ParseNetifReceiveSkb(uint32_t cpu,
                            int64_t timestamp,
                            protozero::ConstBytes);
  void ParseNetDevXmit(uint32_t cpu, int64_t timestamp, protozero::ConstBytes);
  void ParseInetSockSetState(int64_t timestamp,
                             uint32_t pid,
                             protozero::ConstBytes);
  void ParseTcpRetransmitSkb(int64_t timestamp, protozero::ConstBytes);
  void ParseNapiGroReceiveEntry(uint32_t cpu,
                                int64_t timestamp,
                                protozero::ConstBytes);
  void ParseNapiGroReceiveExit(uint32_t cpu,
                               int64_t timestamp,
                               protozero::ConstBytes);
  void ParseCpuFrequencyLimits(int64_t timestamp, protozero::ConstBytes);
  void ParseKfreeSkb(int64_t timestamp, protozero::ConstBytes);
  void ParseUfshcdCommand(int64_t timestamp, protozero::ConstBytes);
  void ParseUfshcdClkGating(int64_t timestamp, protozero::ConstBytes);

  void ParseCrosEcSensorhubData(int64_t timestamp, protozero::ConstBytes);
  void ParseWakeSourceActivate(int64_t timestamp, protozero::ConstBytes);
  void ParseWakeSourceDeactivate(int64_t timestamp, protozero::ConstBytes);
  void ParseSuspendResume(int64_t timestamp, protozero::ConstBytes);

  TraceProcessorContext* context_;
  RssStatTracker rss_stat_tracker_;
  DrmTracker drm_tracker_;

  const StringId sched_wakeup_name_id_;
  const StringId sched_waking_name_id_;
  const StringId cpu_id_;
  const StringId cpu_freq_name_id_;
  const StringId gpu_freq_name_id_;
  const StringId cpu_idle_name_id_;
  const StringId suspend_resume_name_id_;
  const StringId kfree_skb_name_id_;
  const StringId ion_total_id_;
  const StringId ion_change_id_;
  const StringId ion_buffer_id_;
  const StringId dma_heap_total_id_;
  const StringId dma_heap_change_id_;
  const StringId dma_buffer_id_;
  const StringId ion_total_unknown_id_;
  const StringId ion_change_unknown_id_;
  const StringId signal_generate_id_;
  const StringId signal_deliver_id_;
  const StringId oom_score_adj_id_;
  const StringId lmk_id_;
  const StringId comm_name_id_;
  const StringId signal_name_id_;
  const StringId oom_kill_id_;
  const StringId workqueue_id_;
  const StringId irq_id_;
  const StringId tcp_state_id_;
  const StringId tcp_event_id_;
  const StringId protocol_arg_id_;
  const StringId napi_gro_id_;
  const StringId tcp_retransmited_name_id_;
  const StringId ret_arg_id_;
  const StringId len_arg_id_;
  const StringId direct_reclaim_nr_reclaimed_id_;
  const StringId direct_reclaim_order_id_;
  const StringId direct_reclaim_may_writepage_id_;
  const StringId direct_reclaim_gfp_flags_id_;
  const StringId vec_arg_id_;
  const StringId gpu_mem_total_name_id_;
  const StringId gpu_mem_total_unit_id_;
  const StringId gpu_mem_total_global_desc_id_;
  const StringId gpu_mem_total_proc_desc_id_;
  const StringId sched_blocked_reason_id_;
  const StringId io_wait_id_;
  const StringId function_id_;
  const StringId waker_utid_id_;
  const StringId cros_ec_arg_num_id_;
  const StringId cros_ec_arg_ec_id_;
  const StringId cros_ec_arg_sample_ts_id_;
  const StringId ufs_clkgating_id_;
  const StringId ufs_command_count_id_;

  struct FtraceMessageStrings {
    // The string id of name of the event field (e.g. sched_switch's id).
    StringId message_name_id = kNullStringId;
    std::array<StringId, kMaxFtraceEventFields> field_name_ids;
  };
  std::vector<FtraceMessageStrings> ftrace_message_strings_;

  struct MmEventCounterNames {
    MmEventCounterNames() = default;
    MmEventCounterNames(StringId _count, StringId _max_lat, StringId _avg_lat)
        : count(_count), max_lat(_max_lat), avg_lat(_avg_lat) {}

    StringId count = kNullStringId;
    StringId max_lat = kNullStringId;
    StringId avg_lat = kNullStringId;
  };

  static constexpr size_t kFastRpcCounterSize = 4;
  std::array<StringId, kFastRpcCounterSize> fast_rpc_delta_names_;
  std::array<StringId, kFastRpcCounterSize> fast_rpc_total_names_;

  // Keep kMmEventCounterSize equal to mm_event_type::MM_TYPE_NUM in the kernel.
  static constexpr size_t kMmEventCounterSize = 7;
  std::array<MmEventCounterNames, kMmEventCounterSize> mm_event_counter_names_;

  // Record number of received bytes from the network interface card.
  std::unordered_map<StringId, uint64_t> nic_received_bytes_;

  // Record number of transmitted bytes to the network interface card.
  std::unordered_map<StringId, uint64_t> nic_transmitted_bytes_;

  // Record number of kfree_skb with ip protocol.
  uint64_t num_of_kfree_skb_ip_prot = 0;

  // Keep sock to stream number mapping.
  std::unordered_map<uint64_t, uint32_t> skaddr_to_stream_;

  // Record number of tcp steams.
  uint32_t num_of_tcp_stream_ = 0;

  // A name collision is possible, always show if active wakelock exists
  // with a give name
  std::unordered_map<std::string, uint32_t> active_wakelock_to_count_;

  bool has_seen_first_ftrace_packet_ = false;

  // Stores information about the timestamp from the metadata table which is
  // used to filter ftrace packets which happen before this point.
  int64_t drop_ftrace_data_before_ts_ = 0;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_FTRACE_PARSER_H_
