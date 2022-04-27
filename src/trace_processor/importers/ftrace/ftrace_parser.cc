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

#include "src/trace_processor/importers/ftrace/ftrace_parser.h"

#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/ftrace/binder_tracker.h"
#include "src/trace_processor/importers/proto/async_track_set_tracker.h"
#include "src/trace_processor/importers/proto/metadata_tracker.h"
#include "src/trace_processor/importers/syscalls/syscall_tracker.h"
#include "src/trace_processor/importers/systrace/systrace_parser.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/softirq_action.h"
#include "src/trace_processor/types/tcp_state.h"

#include "protos/perfetto/common/gpu_counter_descriptor.pbzero.h"
#include "protos/perfetto/trace/ftrace/binder.pbzero.h"
#include "protos/perfetto/trace/ftrace/cpuhp.pbzero.h"
#include "protos/perfetto/trace/ftrace/cros_ec.pbzero.h"
#include "protos/perfetto/trace/ftrace/dmabuf_heap.pbzero.h"
#include "protos/perfetto/trace/ftrace/dpu.pbzero.h"
#include "protos/perfetto/trace/ftrace/fastrpc.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_stats.pbzero.h"
#include "protos/perfetto/trace/ftrace/g2d.pbzero.h"
#include "protos/perfetto/trace/ftrace/generic.pbzero.h"
#include "protos/perfetto/trace/ftrace/gpu_mem.pbzero.h"
#include "protos/perfetto/trace/ftrace/ion.pbzero.h"
#include "protos/perfetto/trace/ftrace/irq.pbzero.h"
#include "protos/perfetto/trace/ftrace/kmem.pbzero.h"
#include "protos/perfetto/trace/ftrace/lowmemorykiller.pbzero.h"
#include "protos/perfetto/trace/ftrace/mali.pbzero.h"
#include "protos/perfetto/trace/ftrace/mm_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/net.pbzero.h"
#include "protos/perfetto/trace/ftrace/oom.pbzero.h"
#include "protos/perfetto/trace/ftrace/power.pbzero.h"
#include "protos/perfetto/trace/ftrace/raw_syscalls.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/ftrace/scm.pbzero.h"
#include "protos/perfetto/trace/ftrace/sde.pbzero.h"
#include "protos/perfetto/trace/ftrace/signal.pbzero.h"
#include "protos/perfetto/trace/ftrace/skb.pbzero.h"
#include "protos/perfetto/trace/ftrace/sock.pbzero.h"
#include "protos/perfetto/trace/ftrace/systrace.pbzero.h"
#include "protos/perfetto/trace/ftrace/task.pbzero.h"
#include "protos/perfetto/trace/ftrace/tcp.pbzero.h"
#include "protos/perfetto/trace/ftrace/thermal.pbzero.h"
#include "protos/perfetto/trace/ftrace/ufs.pbzero.h"
#include "protos/perfetto/trace/ftrace/vmscan.pbzero.h"
#include "protos/perfetto/trace/ftrace/workqueue.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {

using protozero::ConstBytes;
using protozero::ProtoDecoder;

// kthreadd is the parent process for all kernel threads and always has
// pid == 2 on Linux and Android.
const uint32_t kKthreaddPid = 2;
const char kKthreaddName[] = "kthreadd";

struct FtraceEventAndFieldId {
  uint32_t event_id;
  uint32_t field_id;
};

// Contains a list of all the proto fields in ftrace events which represent
// kernel functions. This list is used to convert the iids in these fields to
// proper kernel symbols.
// TODO(lalitm): going through this array is O(n) on a hot-path (see
// ParseTypedFtraceToRaw). Consider changing this if we end up adding a lot of
// events here.
constexpr auto kKernelFunctionFields = std::array<FtraceEventAndFieldId, 3>{
    {FtraceEventAndFieldId{
         protos::pbzero::FtraceEvent::kSchedBlockedReasonFieldNumber,
         protos::pbzero::SchedBlockedReasonFtraceEvent::kCallerFieldNumber},
     FtraceEventAndFieldId{
         protos::pbzero::FtraceEvent::kWorkqueueExecuteStartFieldNumber,
         protos::pbzero::WorkqueueExecuteStartFtraceEvent::
             kFunctionFieldNumber},
     FtraceEventAndFieldId{
         protos::pbzero::FtraceEvent::kWorkqueueQueueWorkFieldNumber,
         protos::pbzero::WorkqueueQueueWorkFtraceEvent::kFunctionFieldNumber}}};

}  // namespace

FtraceParser::FtraceParser(TraceProcessorContext* context)
    : context_(context),
      rss_stat_tracker_(context),
      sched_wakeup_name_id_(context->storage->InternString("sched_wakeup")),
      sched_waking_name_id_(context->storage->InternString("sched_waking")),
      cpu_id_(context->storage->InternString("cpu")),
      cpu_freq_name_id_(context->storage->InternString("cpufreq")),
      gpu_freq_name_id_(context->storage->InternString("gpufreq")),
      cpu_idle_name_id_(context->storage->InternString("cpuidle")),
      suspend_resume_name_id_(
          context->storage->InternString("Suspend/Resume Latency")),
      kfree_skb_name_id_(context->storage->InternString("Kfree Skb IP Prot")),
      ion_total_id_(context->storage->InternString("mem.ion")),
      ion_change_id_(context->storage->InternString("mem.ion_change")),
      ion_buffer_id_(context->storage->InternString("mem.ion_buffer")),
      dma_heap_total_id_(context->storage->InternString("mem.dma_heap")),
      dma_heap_change_id_(
          context->storage->InternString("mem.dma_heap_change")),
      dma_buffer_id_(context->storage->InternString("mem.dma_buffer")),
      ion_total_unknown_id_(context->storage->InternString("mem.ion.unknown")),
      ion_change_unknown_id_(
          context->storage->InternString("mem.ion_change.unknown")),
      signal_generate_id_(context->storage->InternString("signal_generate")),
      signal_deliver_id_(context->storage->InternString("signal_deliver")),
      oom_score_adj_id_(context->storage->InternString("oom_score_adj")),
      lmk_id_(context->storage->InternString("mem.lmk")),
      comm_name_id_(context->storage->InternString("comm")),
      signal_name_id_(context_->storage->InternString("signal.sig")),
      oom_kill_id_(context_->storage->InternString("mem.oom_kill")),
      workqueue_id_(context_->storage->InternString("workqueue")),
      irq_id_(context_->storage->InternString("irq")),
      tcp_state_id_(context_->storage->InternString("tcp_state")),
      tcp_event_id_(context_->storage->InternString("tcp_event")),
      protocol_arg_id_(context_->storage->InternString("protocol")),
      napi_gro_id_(context_->storage->InternString("napi_gro")),
      tcp_retransmited_name_id_(
          context_->storage->InternString("TCP Retransmit Skb")),
      ret_arg_id_(context_->storage->InternString("ret")),
      len_arg_id_(context->storage->InternString("len")),
      direct_reclaim_nr_reclaimed_id_(
          context->storage->InternString("direct_reclaim_nr_reclaimed")),
      direct_reclaim_order_id_(
          context->storage->InternString("direct_reclaim_order")),
      direct_reclaim_may_writepage_id_(
          context->storage->InternString("direct_reclaim_may_writepage")),
      direct_reclaim_gfp_flags_id_(
          context->storage->InternString("direct_reclaim_gfp_flags")),
      vec_arg_id_(context->storage->InternString("vec")),
      gpu_mem_total_name_id_(context->storage->InternString("GPU Memory")),
      gpu_mem_total_unit_id_(context->storage->InternString(
          std::to_string(protos::pbzero::GpuCounterDescriptor::BYTE).c_str())),
      gpu_mem_total_global_desc_id_(context->storage->InternString(
          "Total GPU memory used by the entire system")),
      gpu_mem_total_proc_desc_id_(context->storage->InternString(
          "Total GPU memory used by this process")),
      sched_blocked_reason_id_(
          context->storage->InternString("sched_blocked_reason")),
      io_wait_id_(context->storage->InternString("io_wait")),
      function_id_(context->storage->InternString("function")),
      waker_utid_id_(context->storage->InternString("waker_utid")),
      cros_ec_arg_num_id_(context->storage->InternString("ec_num")),
      cros_ec_arg_ec_id_(context->storage->InternString("ec_delta")),
      cros_ec_arg_sample_ts_id_(context->storage->InternString("sample_ts")),
      ufs_clkgating_id_(context->storage->InternString(
          "UFS clkgating (OFF/REQ_OFF/REQ_ON/ON)")),
      ufs_command_count_id_(
          context->storage->InternString("UFS Command Count")) {
  // Build the lookup table for the strings inside ftrace events (e.g. the
  // name of ftrace event fields and the names of their args).
  for (size_t i = 0; i < GetDescriptorsSize(); i++) {
    auto* descriptor = GetMessageDescriptorForId(i);
    if (!descriptor->name) {
      ftrace_message_strings_.emplace_back();
      continue;
    }

    FtraceMessageStrings ftrace_strings;
    ftrace_strings.message_name_id =
        context->storage->InternString(descriptor->name);

    for (size_t fid = 0; fid <= descriptor->max_field_id; fid++) {
      const auto& field = descriptor->fields[fid];
      if (!field.name)
        continue;
      ftrace_strings.field_name_ids[fid] =
          context->storage->InternString(field.name);
    }
    ftrace_message_strings_.emplace_back(ftrace_strings);
  }

  // Array initialization causes a spurious warning due to llvm bug.
  // See https://bugs.llvm.org/show_bug.cgi?id=21629
  fast_rpc_delta_names_[0] =
      context->storage->InternString("mem.fastrpc_change[ASDP]");
  fast_rpc_delta_names_[1] =
      context->storage->InternString("mem.fastrpc_change[MDSP]");
  fast_rpc_delta_names_[2] =
      context->storage->InternString("mem.fastrpc_change[SDSP]");
  fast_rpc_delta_names_[3] =
      context->storage->InternString("mem.fastrpc_change[CDSP]");
  fast_rpc_total_names_[0] =
      context->storage->InternString("mem.fastrpc[ASDP]");
  fast_rpc_total_names_[1] =
      context->storage->InternString("mem.fastrpc[MDSP]");
  fast_rpc_total_names_[2] =
      context->storage->InternString("mem.fastrpc[SDSP]");
  fast_rpc_total_names_[3] =
      context->storage->InternString("mem.fastrpc[CDSP]");

  mm_event_counter_names_ = {
      {MmEventCounterNames(
           context->storage->InternString("mem.mm.min_flt.count"),
           context->storage->InternString("mem.mm.min_flt.max_lat"),
           context->storage->InternString("mem.mm.min_flt.avg_lat")),
       MmEventCounterNames(
           context->storage->InternString("mem.mm.maj_flt.count"),
           context->storage->InternString("mem.mm.maj_flt.max_lat"),
           context->storage->InternString("mem.mm.maj_flt.avg_lat")),
       MmEventCounterNames(
           context->storage->InternString("mem.mm.read_io.count"),
           context->storage->InternString("mem.mm.read_io.max_lat"),
           context->storage->InternString("mem.mm.read_io.avg_lat")),
       MmEventCounterNames(
           context->storage->InternString("mem.mm.compaction.count"),
           context->storage->InternString("mem.mm.compaction.max_lat"),
           context->storage->InternString("mem.mm.compaction.avg_lat")),
       MmEventCounterNames(
           context->storage->InternString("mem.mm.reclaim.count"),
           context->storage->InternString("mem.mm.reclaim.max_lat"),
           context->storage->InternString("mem.mm.reclaim.avg_lat")),
       MmEventCounterNames(
           context->storage->InternString("mem.mm.swp_flt.count"),
           context->storage->InternString("mem.mm.swp_flt.max_lat"),
           context->storage->InternString("mem.mm.swp_flt.avg_lat")),
       MmEventCounterNames(
           context->storage->InternString("mem.mm.kern_alloc.count"),
           context->storage->InternString("mem.mm.kern_alloc.max_lat"),
           context->storage->InternString("mem.mm.kern_alloc.avg_lat"))}};
}

void FtraceParser::ParseFtraceStats(ConstBytes blob) {
  protos::pbzero::FtraceStats::Decoder evt(blob.data, blob.size);
  bool is_start =
      evt.phase() == protos::pbzero::FtraceStats_Phase_START_OF_TRACE;
  bool is_end = evt.phase() == protos::pbzero::FtraceStats_Phase_END_OF_TRACE;
  if (!is_start && !is_end) {
    PERFETTO_ELOG("Ignoring unknown ftrace stats phase %d", evt.phase());
    return;
  }
  size_t phase = is_end ? 1 : 0;

  // This code relies on the fact that each ftrace_cpu_XXX_end event is
  // just after the corresponding ftrace_cpu_XXX_begin event.
  static_assert(
      stats::ftrace_cpu_read_events_end - stats::ftrace_cpu_read_events_begin ==
              1 &&
          stats::ftrace_cpu_entries_end - stats::ftrace_cpu_entries_begin == 1,
      "ftrace_cpu_XXX stats definition are messed up");

  auto* storage = context_->storage.get();
  for (auto it = evt.cpu_stats(); it; ++it) {
    protos::pbzero::FtraceCpuStats::Decoder cpu_stats(*it);
    int cpu = static_cast<int>(cpu_stats.cpu());

    int64_t entries = static_cast<int64_t>(cpu_stats.entries());
    int64_t overrun = static_cast<int64_t>(cpu_stats.overrun());
    int64_t commit_overrun = static_cast<int64_t>(cpu_stats.commit_overrun());
    int64_t bytes_read = static_cast<int64_t>(cpu_stats.bytes_read());
    int64_t dropped_events = static_cast<int64_t>(cpu_stats.dropped_events());
    int64_t read_events = static_cast<int64_t>(cpu_stats.read_events());
    int64_t now_ts = static_cast<int64_t>(cpu_stats.now_ts() * 1e9);

    storage->SetIndexedStats(stats::ftrace_cpu_entries_begin + phase, cpu,
                             entries);
    storage->SetIndexedStats(stats::ftrace_cpu_overrun_begin + phase, cpu,
                             overrun);
    storage->SetIndexedStats(stats::ftrace_cpu_commit_overrun_begin + phase,
                             cpu, commit_overrun);
    storage->SetIndexedStats(stats::ftrace_cpu_bytes_read_begin + phase, cpu,
                             bytes_read);
    storage->SetIndexedStats(stats::ftrace_cpu_dropped_events_begin + phase,
                             cpu, dropped_events);
    storage->SetIndexedStats(stats::ftrace_cpu_read_events_begin + phase, cpu,
                             read_events);
    storage->SetIndexedStats(stats::ftrace_cpu_now_ts_begin + phase, cpu,
                             now_ts);

    if (is_end) {
      auto opt_entries_begin =
          storage->GetIndexedStats(stats::ftrace_cpu_entries_begin, cpu);
      if (opt_entries_begin) {
        int64_t delta_entries = entries - opt_entries_begin.value();
        storage->SetIndexedStats(stats::ftrace_cpu_entries_delta, cpu,
                                 delta_entries);
      }

      auto opt_overrun_begin =
          storage->GetIndexedStats(stats::ftrace_cpu_overrun_begin, cpu);
      if (opt_overrun_begin) {
        int64_t delta_overrun = overrun - opt_overrun_begin.value();
        storage->SetIndexedStats(stats::ftrace_cpu_overrun_delta, cpu,
                                 delta_overrun);
      }

      auto opt_commit_overrun_begin =
          storage->GetIndexedStats(stats::ftrace_cpu_commit_overrun_begin, cpu);
      if (opt_commit_overrun_begin) {
        int64_t delta_commit_overrun =
            commit_overrun - opt_commit_overrun_begin.value();
        storage->SetIndexedStats(stats::ftrace_cpu_commit_overrun_delta, cpu,
                                 delta_commit_overrun);
      }

      auto opt_bytes_read_begin =
          storage->GetIndexedStats(stats::ftrace_cpu_bytes_read_begin, cpu);
      if (opt_bytes_read_begin) {
        int64_t delta_bytes_read = bytes_read - opt_bytes_read_begin.value();
        storage->SetIndexedStats(stats::ftrace_cpu_bytes_read_delta, cpu,
                                 delta_bytes_read);
      }

      auto opt_dropped_events_begin =
          storage->GetIndexedStats(stats::ftrace_cpu_dropped_events_begin, cpu);
      if (opt_dropped_events_begin) {
        int64_t delta_dropped_events =
            dropped_events - opt_dropped_events_begin.value();
        storage->SetIndexedStats(stats::ftrace_cpu_dropped_events_delta, cpu,
                                 delta_dropped_events);
      }

      auto opt_read_events_begin =
          storage->GetIndexedStats(stats::ftrace_cpu_read_events_begin, cpu);
      if (opt_read_events_begin) {
        int64_t delta_read_events = read_events - opt_read_events_begin.value();
        storage->SetIndexedStats(stats::ftrace_cpu_read_events_delta, cpu,
                                 delta_read_events);
      }
    }

    // oldest_event_ts can often be set to very high values, possibly because
    // of wrapping. Ensure that we are not overflowing to avoid ubsan
    // complaining.
    double oldest_event_ts = cpu_stats.oldest_event_ts() * 1e9;
    // NB: This comparison is correct only because of the >=, it would be
    // incorrect with >. std::numeric_limits<int64_t>::max() converted to
    // a double is the next value representable as a double that is *larger*
    // than std::numeric_limits<int64_t>::max(). All values that are
    // representable as doubles and < than that value are thus representable
    // as int64_t.
    if (oldest_event_ts >=
        static_cast<double>(std::numeric_limits<int64_t>::max())) {
      storage->SetIndexedStats(stats::ftrace_cpu_oldest_event_ts_begin + phase,
                               cpu, std::numeric_limits<int64_t>::max());
    } else {
      storage->SetIndexedStats(stats::ftrace_cpu_oldest_event_ts_begin + phase,
                               cpu, static_cast<int64_t>(oldest_event_ts));
    }
  }

  // Compute atrace + ftrace setup errors. We do two things here:
  // 1. We add up all the errors and put the counter in the stats table (which
  //    can hold only numerals). This will raise an orange flag in the UI.
  // 2. We concatenate together all the errors in a string and put that in the
  //    medatata table.
  // Both will be reported in the 'Info & stats' page in the UI.
  if (is_start) {
    std::string error_str;
    for (auto it = evt.failed_ftrace_events(); it; ++it) {
      storage->IncrementStats(stats::ftrace_setup_errors, 1);
      error_str += "Ftrace event failed: " + it->as_std_string() + "\n";
    }
    for (auto it = evt.unknown_ftrace_events(); it; ++it) {
      storage->IncrementStats(stats::ftrace_setup_errors, 1);
      error_str += "Ftrace event unknown: " + it->as_std_string() + "\n";
    }
    if (evt.atrace_errors().size > 0) {
      storage->IncrementStats(stats::ftrace_setup_errors, 1);
      error_str += "Atrace failures: " + evt.atrace_errors().ToStdString();
    }
    auto error_str_id = storage->InternString(base::StringView(error_str));
    context_->metadata_tracker->SetMetadata(metadata::ftrace_setup_errors,
                                            Variadic::String(error_str_id));
  }
}

PERFETTO_ALWAYS_INLINE
util::Status FtraceParser::ParseFtraceEvent(uint32_t cpu,
                                            const TimestampedTracePiece& ttp) {
  int64_t ts = ttp.timestamp;

  // On the first ftrace packet, check the metadata table for the
  // ts of the event which is specified in the config. If it exists we can use
  // it to filter out ftrace packets which happen earlier than it.
  if (PERFETTO_UNLIKELY(!has_seen_first_ftrace_packet_)) {
    DropFtraceDataBefore drop_before = context_->config.drop_ftrace_data_before;
    switch (drop_before) {
      case DropFtraceDataBefore::kNoDrop: {
        drop_ftrace_data_before_ts_ = 0;
        break;
      }
      case DropFtraceDataBefore::kAllDataSourcesStarted:
      case DropFtraceDataBefore::kTracingStarted: {
        metadata::KeyId event_key =
            drop_before == DropFtraceDataBefore::kAllDataSourcesStarted
                ? metadata::all_data_source_started_ns
                : metadata::tracing_started_ns;
        const auto& metadata = context_->storage->metadata_table();
        base::Optional<uint32_t> opt_row =
            metadata.name().IndexOf(metadata::kNames[event_key]);
        if (opt_row) {
          drop_ftrace_data_before_ts_ = *metadata.int_value()[*opt_row];
        }
        break;
      }
    }
    has_seen_first_ftrace_packet_ = true;
  }

  if (PERFETTO_UNLIKELY(ts < drop_ftrace_data_before_ts_)) {
    context_->storage->IncrementStats(
        stats::ftrace_packet_before_tracing_start);
    return util::OkStatus();
  }

  using protos::pbzero::FtraceEvent;
  SchedEventTracker* sched_tracker = SchedEventTracker::GetOrCreate(context_);

  // Handle the (optional) alternative encoding format for sched_switch.
  if (ttp.type == TimestampedTracePiece::Type::kInlineSchedSwitch) {
    const auto& event = ttp.sched_switch;
    sched_tracker->PushSchedSwitchCompact(cpu, ts, event.prev_state,
                                          static_cast<uint32_t>(event.next_pid),
                                          event.next_prio, event.next_comm);
    return util::OkStatus();
  }

  // Handle the (optional) alternative encoding format for sched_waking.
  if (ttp.type == TimestampedTracePiece::Type::kInlineSchedWaking) {
    const auto& event = ttp.sched_waking;
    sched_tracker->PushSchedWakingCompact(
        cpu, ts, static_cast<uint32_t>(event.pid), event.target_cpu, event.prio,
        event.comm);
    return util::OkStatus();
  }

  PERFETTO_DCHECK(ttp.type == TimestampedTracePiece::Type::kFtraceEvent);
  const TraceBlobView& event = ttp.ftrace_event.event;
  PacketSequenceStateGeneration* seq_state =
      ttp.ftrace_event.sequence_state.get();
  ProtoDecoder decoder(event.data(), event.length());
  uint64_t raw_pid = 0;
  if (auto pid_field = decoder.FindField(FtraceEvent::kPidFieldNumber)) {
    raw_pid = pid_field.as_uint64();
  } else {
    return util::ErrStatus("Pid field not found in ftrace packet");
  }
  uint32_t pid = static_cast<uint32_t>(raw_pid);

  for (auto fld = decoder.ReadField(); fld.valid(); fld = decoder.ReadField()) {
    bool is_metadata_field = fld.id() == FtraceEvent::kPidFieldNumber ||
                             fld.id() == FtraceEvent::kTimestampFieldNumber;
    if (is_metadata_field)
      continue;

    ConstBytes data = fld.as_bytes();
    if (fld.id() == FtraceEvent::kGenericFieldNumber) {
      ParseGenericFtrace(ts, cpu, pid, data);
    } else if (fld.id() != FtraceEvent::kSchedSwitchFieldNumber) {
      // sched_switch parsing populates the raw table by itself
      ParseTypedFtraceToRaw(fld.id(), ts, cpu, pid, data, seq_state);
    }

    switch (fld.id()) {
      case FtraceEvent::kSchedSwitchFieldNumber: {
        ParseSchedSwitch(cpu, ts, data);
        break;
      }
      case FtraceEvent::kSchedWakeupFieldNumber: {
        ParseSchedWakeup(ts, pid, data);
        break;
      }
      case FtraceEvent::kSchedWakingFieldNumber: {
        ParseSchedWaking(ts, pid, data);
        break;
      }
      case FtraceEvent::kSchedProcessFreeFieldNumber: {
        ParseSchedProcessFree(ts, data);
        break;
      }
      case FtraceEvent::kCpuFrequencyFieldNumber: {
        ParseCpuFreq(ts, data);
        break;
      }
      case FtraceEvent::kGpuFrequencyFieldNumber: {
        ParseGpuFreq(ts, data);
        break;
      }
      case FtraceEvent::kCpuIdleFieldNumber: {
        ParseCpuIdle(ts, data);
        break;
      }
      case FtraceEvent::kPrintFieldNumber: {
        ParsePrint(ts, pid, data);
        break;
      }
      case FtraceEvent::kZeroFieldNumber: {
        ParseZero(ts, pid, data);
        break;
      }
      case FtraceEvent::kRssStatThrottledFieldNumber:
      case FtraceEvent::kRssStatFieldNumber: {
        rss_stat_tracker_.ParseRssStat(ts, fld.id(), pid, data);
        break;
      }
      case FtraceEvent::kIonHeapGrowFieldNumber: {
        ParseIonHeapGrowOrShrink(ts, pid, data, true);
        break;
      }
      case FtraceEvent::kIonHeapShrinkFieldNumber: {
        ParseIonHeapGrowOrShrink(ts, pid, data, false);
        break;
      }
      case FtraceEvent::kIonStatFieldNumber: {
        ParseIonStat(ts, pid, data);
        break;
      }
      case FtraceEvent::kDmaHeapStatFieldNumber: {
        ParseDmaHeapStat(ts, pid, data);
        break;
      }
      case FtraceEvent::kSignalGenerateFieldNumber: {
        ParseSignalGenerate(ts, data);
        break;
      }
      case FtraceEvent::kSignalDeliverFieldNumber: {
        ParseSignalDeliver(ts, pid, data);
        break;
      }
      case FtraceEvent::kOomScoreAdjUpdateFieldNumber: {
        ParseOOMScoreAdjUpdate(ts, data);
        break;
      }
      case FtraceEvent::kMarkVictimFieldNumber: {
        ParseOOMKill(ts, data);
        break;
      }
      case FtraceEvent::kMmEventRecordFieldNumber: {
        ParseMmEventRecord(ts, pid, data);
        break;
      }
      case FtraceEvent::kSysEnterFieldNumber: {
        ParseSysEvent(ts, pid, true, data);
        break;
      }
      case FtraceEvent::kSysExitFieldNumber: {
        ParseSysEvent(ts, pid, false, data);
        break;
      }
      case FtraceEvent::kTaskNewtaskFieldNumber: {
        ParseTaskNewTask(ts, pid, data);
        break;
      }
      case FtraceEvent::kTaskRenameFieldNumber: {
        ParseTaskRename(data);
        break;
      }
      case FtraceEvent::kBinderTransactionFieldNumber: {
        ParseBinderTransaction(ts, pid, data);
        break;
      }
      case FtraceEvent::kBinderTransactionReceivedFieldNumber: {
        ParseBinderTransactionReceived(ts, pid, data);
        break;
      }
      case FtraceEvent::kBinderTransactionAllocBufFieldNumber: {
        ParseBinderTransactionAllocBuf(ts, pid, data);
        break;
      }
      case FtraceEvent::kBinderLockFieldNumber: {
        ParseBinderLock(ts, pid, data);
        break;
      }
      case FtraceEvent::kBinderUnlockFieldNumber: {
        ParseBinderUnlock(ts, pid, data);
        break;
      }
      case FtraceEvent::kBinderLockedFieldNumber: {
        ParseBinderLocked(ts, pid, data);
        break;
      }
      case FtraceEvent::kSdeTracingMarkWriteFieldNumber: {
        ParseSdeTracingMarkWrite(ts, pid, data);
        break;
      }
      case FtraceEvent::kClockSetRateFieldNumber: {
        ParseClockSetRate(ts, data);
        break;
      }
      case FtraceEvent::kClockEnableFieldNumber: {
        ParseClockEnable(ts, data);
        break;
      }
      case FtraceEvent::kClockDisableFieldNumber: {
        ParseClockDisable(ts, data);
        break;
      }
      case FtraceEvent::kScmCallStartFieldNumber: {
        ParseScmCallStart(ts, pid, data);
        break;
      }
      case FtraceEvent::kScmCallEndFieldNumber: {
        ParseScmCallEnd(ts, pid, data);
        break;
      }
      case FtraceEvent::kMmVmscanDirectReclaimBeginFieldNumber: {
        ParseDirectReclaimBegin(ts, pid, data);
        break;
      }
      case FtraceEvent::kMmVmscanDirectReclaimEndFieldNumber: {
        ParseDirectReclaimEnd(ts, pid, data);
        break;
      }
      case FtraceEvent::kWorkqueueExecuteStartFieldNumber: {
        ParseWorkqueueExecuteStart(cpu, ts, pid, data, seq_state);
        break;
      }
      case FtraceEvent::kWorkqueueExecuteEndFieldNumber: {
        ParseWorkqueueExecuteEnd(ts, pid, data);
        break;
      }
      case FtraceEvent::kIrqHandlerEntryFieldNumber: {
        ParseIrqHandlerEntry(cpu, ts, data);
        break;
      }
      case FtraceEvent::kIrqHandlerExitFieldNumber: {
        ParseIrqHandlerExit(cpu, ts, data);
        break;
      }
      case FtraceEvent::kSoftirqEntryFieldNumber: {
        ParseSoftIrqEntry(cpu, ts, data);
        break;
      }
      case FtraceEvent::kSoftirqExitFieldNumber: {
        ParseSoftIrqExit(cpu, ts, data);
        break;
      }
      case FtraceEvent::kGpuMemTotalFieldNumber: {
        ParseGpuMemTotal(ts, data);
        break;
      }
      case FtraceEvent::kThermalTemperatureFieldNumber: {
        ParseThermalTemperature(ts, data);
        break;
      }
      case FtraceEvent::kCdevUpdateFieldNumber: {
        ParseCdevUpdate(ts, data);
        break;
      }
      case FtraceEvent::kSchedBlockedReasonFieldNumber: {
        ParseSchedBlockedReason(ts, data, seq_state);
        break;
      }
      case FtraceEvent::kFastrpcDmaStatFieldNumber: {
        ParseFastRpcDmaStat(ts, pid, data);
        break;
      }
      case FtraceEvent::kG2dTracingMarkWriteFieldNumber: {
        ParseG2dTracingMarkWrite(ts, pid, data);
        break;
      }
      case FtraceEvent::kDpuTracingMarkWriteFieldNumber: {
        ParseDpuTracingMarkWrite(ts, pid, data);
        break;
      }
      case FtraceEvent::kMaliTracingMarkWriteFieldNumber: {
        ParseMaliTracingMarkWrite(ts, pid, data);
        break;
      }
      case FtraceEvent::kCpuhpPauseFieldNumber: {
        ParseCpuhpPause(ts, pid, data);
        break;
      }
      case FtraceEvent::kNetifReceiveSkbFieldNumber: {
        ParseNetifReceiveSkb(cpu, ts, data);
        break;
      }
      case FtraceEvent::kNetDevXmitFieldNumber: {
        ParseNetDevXmit(cpu, ts, data);
        break;
      }
      case FtraceEvent::kInetSockSetStateFieldNumber: {
        ParseInetSockSetState(ts, pid, data);
        break;
      }
      case FtraceEvent::kTcpRetransmitSkbFieldNumber: {
        ParseTcpRetransmitSkb(ts, data);
        break;
      }
      case FtraceEvent::kNapiGroReceiveEntryFieldNumber: {
        ParseNapiGroReceiveEntry(cpu, ts, data);
        break;
      }
      case FtraceEvent::kNapiGroReceiveExitFieldNumber: {
        ParseNapiGroReceiveExit(cpu, ts, data);
        break;
      }
      case FtraceEvent::kCpuFrequencyLimitsFieldNumber: {
        ParseCpuFrequencyLimits(ts, data);
        break;
      }
      case FtraceEvent::kKfreeSkbFieldNumber: {
        ParseKfreeSkb(ts, data);
        break;
      }
      case FtraceEvent::kCrosEcSensorhubDataFieldNumber: {
        ParseCrosEcSensorhubData(ts, data);
        break;
      }
      case FtraceEvent::kUfshcdCommandFieldNumber: {
        ParseUfshcdCommand(ts, data);
        break;
      }
      case FtraceEvent::kWakeupSourceActivateFieldNumber: {
        ParseWakeSourceActivate(ts, data);
        break;
      }
      case FtraceEvent::kWakeupSourceDeactivateFieldNumber: {
        ParseWakeSourceDeactivate(ts, data);
        break;
      }
      case FtraceEvent::kUfshcdClkGatingFieldNumber: {
        ParseUfshcdClkGating(ts, data);
        break;
      }
      case FtraceEvent::kSuspendResumeFieldNumber: {
        ParseSuspendResume(ts, data);
        break;
      }
      default:
        break;
    }
  }

  PERFETTO_DCHECK(!decoder.bytes_left());
  return util::OkStatus();
}

void FtraceParser::ParseGenericFtrace(int64_t ts,
                                      uint32_t cpu,
                                      uint32_t tid,
                                      ConstBytes blob) {
  protos::pbzero::GenericFtraceEvent::Decoder evt(blob.data, blob.size);
  StringId event_id = context_->storage->InternString(evt.event_name());
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(tid);
  RawId id = context_->storage->mutable_raw_table()
                 ->Insert({ts, event_id, cpu, utid})
                 .id;
  auto inserter = context_->args_tracker->AddArgsTo(id);

  for (auto it = evt.field(); it; ++it) {
    protos::pbzero::GenericFtraceEvent::Field::Decoder fld(*it);
    auto field_name_id = context_->storage->InternString(fld.name());
    if (fld.has_int_value()) {
      inserter.AddArg(field_name_id, Variadic::Integer(fld.int_value()));
    } else if (fld.has_uint_value()) {
      inserter.AddArg(
          field_name_id,
          Variadic::Integer(static_cast<int64_t>(fld.uint_value())));
    } else if (fld.has_str_value()) {
      StringId str_value = context_->storage->InternString(fld.str_value());
      inserter.AddArg(field_name_id, Variadic::String(str_value));
    }
  }
}

void FtraceParser::ParseTypedFtraceToRaw(
    uint32_t ftrace_id,
    int64_t timestamp,
    uint32_t cpu,
    uint32_t tid,
    ConstBytes blob,
    PacketSequenceStateGeneration* seq_state) {
  if (PERFETTO_UNLIKELY(!context_->config.ingest_ftrace_in_raw_table))
    return;

  ProtoDecoder decoder(blob.data, blob.size);
  if (ftrace_id >= GetDescriptorsSize()) {
    PERFETTO_DLOG("Event with id: %d does not exist and cannot be parsed.",
                  ftrace_id);
    return;
  }

  MessageDescriptor* m = GetMessageDescriptorForId(ftrace_id);
  const auto& message_strings = ftrace_message_strings_[ftrace_id];
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(tid);
  RawId id =
      context_->storage->mutable_raw_table()
          ->Insert({timestamp, message_strings.message_name_id, cpu, utid})
          .id;
  auto inserter = context_->args_tracker->AddArgsTo(id);

  for (auto fld = decoder.ReadField(); fld.valid(); fld = decoder.ReadField()) {
    uint16_t field_id = fld.id();
    if (PERFETTO_UNLIKELY(field_id >= kMaxFtraceEventFields)) {
      PERFETTO_DLOG(
          "Skipping ftrace arg - proto field id is too large (%" PRIu16 ")",
          field_id);
      continue;
    }

    ProtoSchemaType type = m->fields[field_id].type;
    StringId name_id = message_strings.field_name_ids[field_id];

    // Check if this field represents a kernel function.
    auto it = std::find_if(
        kKernelFunctionFields.begin(), kKernelFunctionFields.end(),
        [ftrace_id, field_id](const FtraceEventAndFieldId& ev) {
          return ev.event_id == ftrace_id && ev.field_id == field_id;
        });
    if (it != kKernelFunctionFields.end()) {
      PERFETTO_CHECK(type == ProtoSchemaType::kUint64);

      auto* interned_string = seq_state->LookupInternedMessage<
          protos::pbzero::InternedData::kKernelSymbolsFieldNumber,
          protos::pbzero::InternedString>(fld.as_uint64());

      // If we don't have the string for this field (can happen if
      // symbolization wasn't enabled, if reading the symbols errored out or
      // on legacy traces) then just add the field as a normal arg.
      if (interned_string) {
        protozero::ConstBytes str = interned_string->str();
        StringId str_id = context_->storage->InternString(base::StringView(
            reinterpret_cast<const char*>(str.data), str.size));
        inserter.AddArg(name_id, Variadic::String(str_id));
        continue;
      }
    }

    switch (type) {
      case ProtoSchemaType::kInt32:
      case ProtoSchemaType::kInt64:
      case ProtoSchemaType::kSfixed32:
      case ProtoSchemaType::kSfixed64:
      case ProtoSchemaType::kSint32:
      case ProtoSchemaType::kSint64:
      case ProtoSchemaType::kBool:
      case ProtoSchemaType::kEnum: {
        inserter.AddArg(name_id, Variadic::Integer(fld.as_int64()));
        break;
      }
      case ProtoSchemaType::kUint32:
      case ProtoSchemaType::kUint64:
      case ProtoSchemaType::kFixed32:
      case ProtoSchemaType::kFixed64: {
        // Note that SQLite functions will still treat unsigned values
        // as a signed 64 bit integers (but the translation back to ftrace
        // refers to this storage directly).
        inserter.AddArg(name_id, Variadic::UnsignedInteger(fld.as_uint64()));
        break;
      }
      case ProtoSchemaType::kString:
      case ProtoSchemaType::kBytes: {
        StringId value = context_->storage->InternString(fld.as_string());
        inserter.AddArg(name_id, Variadic::String(value));
        break;
      }
      case ProtoSchemaType::kDouble: {
        inserter.AddArg(name_id, Variadic::Real(fld.as_double()));
        break;
      }
      case ProtoSchemaType::kFloat: {
        inserter.AddArg(name_id,
                        Variadic::Real(static_cast<double>(fld.as_float())));
        break;
      }
      case ProtoSchemaType::kUnknown:
      case ProtoSchemaType::kGroup:
      case ProtoSchemaType::kMessage:
        PERFETTO_DLOG("Could not store %s as a field in args table.",
                      ProtoSchemaToString(type));
        break;
    }
  }
}

PERFETTO_ALWAYS_INLINE
void FtraceParser::ParseSchedSwitch(uint32_t cpu,
                                    int64_t timestamp,
                                    ConstBytes blob) {
  protos::pbzero::SchedSwitchFtraceEvent::Decoder ss(blob.data, blob.size);
  uint32_t prev_pid = static_cast<uint32_t>(ss.prev_pid());
  uint32_t next_pid = static_cast<uint32_t>(ss.next_pid());
  SchedEventTracker::GetOrCreate(context_)->PushSchedSwitch(
      cpu, timestamp, prev_pid, ss.prev_comm(), ss.prev_prio(), ss.prev_state(),
      next_pid, ss.next_comm(), ss.next_prio());
}

void FtraceParser::ParseSchedWakeup(int64_t timestamp,
                                    uint32_t pid,
                                    ConstBytes blob) {
  protos::pbzero::SchedWakeupFtraceEvent::Decoder sw(blob.data, blob.size);
  uint32_t wakee_pid = static_cast<uint32_t>(sw.pid());
  StringId name_id = context_->storage->InternString(sw.comm());
  auto wakee_utid = context_->process_tracker->UpdateThreadName(
      wakee_pid, name_id, ThreadNamePriority::kFtrace);
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  auto instant_id = context_->storage->mutable_legacy_instant_table()->Insert(
      {timestamp, sched_wakeup_name_id_, wakee_utid});
  context_->args_tracker->AddArgsTo(instant_id.id)
      .AddArg(waker_utid_id_, Variadic::UnsignedInteger(utid));
}

void FtraceParser::ParseSchedWaking(int64_t timestamp,
                                    uint32_t pid,
                                    ConstBytes blob) {
  protos::pbzero::SchedWakingFtraceEvent::Decoder sw(blob.data, blob.size);
  uint32_t wakee_pid = static_cast<uint32_t>(sw.pid());
  StringId name_id = context_->storage->InternString(sw.comm());
  auto wakee_utid = context_->process_tracker->UpdateThreadName(
      wakee_pid, name_id, ThreadNamePriority::kFtrace);
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  auto instant_id = context_->storage->mutable_legacy_instant_table()->Insert(
      {timestamp, sched_waking_name_id_, wakee_utid});
  context_->args_tracker->AddArgsTo(instant_id.id)
      .AddArg(waker_utid_id_, Variadic::UnsignedInteger(utid));
}

void FtraceParser::ParseSchedProcessFree(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::SchedProcessFreeFtraceEvent::Decoder ex(blob.data, blob.size);
  uint32_t pid = static_cast<uint32_t>(ex.pid());
  context_->process_tracker->EndThread(timestamp, pid);
}

void FtraceParser::ParseCpuFreq(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::CpuFrequencyFtraceEvent::Decoder freq(blob.data, blob.size);
  uint32_t cpu = freq.cpu_id();
  uint32_t new_freq = freq.state();
  TrackId track =
      context_->track_tracker->InternCpuCounterTrack(cpu_freq_name_id_, cpu);
  context_->event_tracker->PushCounter(timestamp, new_freq, track);
}

void FtraceParser::ParseGpuFreq(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::GpuFrequencyFtraceEvent::Decoder freq(blob.data, blob.size);
  uint32_t gpu = freq.gpu_id();
  uint32_t new_freq = freq.state();
  TrackId track =
      context_->track_tracker->InternGpuCounterTrack(gpu_freq_name_id_, gpu);
  context_->event_tracker->PushCounter(timestamp, new_freq, track);
}

void FtraceParser::ParseCpuIdle(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::CpuIdleFtraceEvent::Decoder idle(blob.data, blob.size);
  uint32_t cpu = idle.cpu_id();
  uint32_t new_state = idle.state();
  TrackId track =
      context_->track_tracker->InternCpuCounterTrack(cpu_idle_name_id_, cpu);
  context_->event_tracker->PushCounter(timestamp, new_state, track);
}

void FtraceParser::ParsePrint(int64_t timestamp,
                              uint32_t pid,
                              ConstBytes blob) {
  protos::pbzero::PrintFtraceEvent::Decoder evt(blob.data, blob.size);
  SystraceParser::GetOrCreate(context_)->ParsePrintEvent(timestamp, pid,
                                                         evt.buf());
}

void FtraceParser::ParseZero(int64_t timestamp, uint32_t pid, ConstBytes blob) {
  protos::pbzero::ZeroFtraceEvent::Decoder evt(blob.data, blob.size);
  uint32_t tgid = static_cast<uint32_t>(evt.pid());
  SystraceParser::GetOrCreate(context_)->ParseZeroEvent(
      timestamp, pid, evt.flag(), evt.name(), tgid, evt.value());
}

void FtraceParser::ParseSdeTracingMarkWrite(int64_t timestamp,
                                            uint32_t pid,
                                            ConstBytes blob) {
  protos::pbzero::SdeTracingMarkWriteFtraceEvent::Decoder evt(blob.data,
                                                              blob.size);
  if (!evt.has_trace_type() && !evt.has_trace_begin()) {
    context_->storage->IncrementStats(stats::systrace_parse_failure);
    return;
  }

  uint32_t tgid = static_cast<uint32_t>(evt.pid());
  SystraceParser::GetOrCreate(context_)->ParseTracingMarkWrite(
      timestamp, pid, static_cast<char>(evt.trace_type()), evt.trace_begin(),
      evt.trace_name(), tgid, evt.value());
}

void FtraceParser::ParseDpuTracingMarkWrite(int64_t timestamp,
                                            uint32_t pid,
                                            ConstBytes blob) {
  protos::pbzero::DpuTracingMarkWriteFtraceEvent::Decoder evt(blob.data,
                                                              blob.size);
  if (!evt.type()) {
    context_->storage->IncrementStats(stats::systrace_parse_failure);
    return;
  }

  uint32_t tgid = static_cast<uint32_t>(evt.pid());
  // For kernel counter events, they will become thread counter tracks.
  // But, we want to use the pid field specified in the event as the thread ID
  // of the thread_counter_track instead of using the thread ID that emitted
  // the events. So here, we need to override pid = tgid.
  if (static_cast<char>(evt.type()) == 'C') {
    pid = tgid;
  }
  SystraceParser::GetOrCreate(context_)->ParseTracingMarkWrite(
      timestamp, pid, static_cast<char>(evt.type()), false /*trace_begin*/,
      evt.name(), tgid, evt.value());
}

void FtraceParser::ParseG2dTracingMarkWrite(int64_t timestamp,
                                            uint32_t pid,
                                            ConstBytes blob) {
  protos::pbzero::G2dTracingMarkWriteFtraceEvent::Decoder evt(blob.data,
                                                              blob.size);
  if (!evt.type()) {
    context_->storage->IncrementStats(stats::systrace_parse_failure);
    return;
  }

  uint32_t tgid = static_cast<uint32_t>(evt.pid());
  // For kernel counter events, they will become thread counter tracks.
  // But, we want to use the pid field specified in the event as the thread ID
  // of the thread_counter_track instead of using the thread ID that emitted
  // the events. So here, we need to override pid = tgid.
  if (static_cast<char>(evt.type()) == 'C') {
    pid = tgid;
  }
  SystraceParser::GetOrCreate(context_)->ParseTracingMarkWrite(
      timestamp, pid, static_cast<char>(evt.type()), false /*trace_begin*/,
      evt.name(), tgid, evt.value());
}

void FtraceParser::ParseMaliTracingMarkWrite(int64_t timestamp,
                                             uint32_t pid,
                                             ConstBytes blob) {
  protos::pbzero::MaliTracingMarkWriteFtraceEvent::Decoder evt(blob.data,
                                                               blob.size);
  if (!evt.type()) {
    context_->storage->IncrementStats(stats::systrace_parse_failure);
    return;
  }

  uint32_t tgid = static_cast<uint32_t>(evt.pid());
  SystraceParser::GetOrCreate(context_)->ParseTracingMarkWrite(
      timestamp, pid, static_cast<char>(evt.type()), false /*trace_begin*/,
      evt.name(), tgid, evt.value());
}

/** Parses ion heap events present in Pixel kernels. */
void FtraceParser::ParseIonHeapGrowOrShrink(int64_t timestamp,
                                            uint32_t pid,
                                            ConstBytes blob,
                                            bool grow) {
  protos::pbzero::IonHeapGrowFtraceEvent::Decoder ion(blob.data, blob.size);
  int64_t change_bytes = static_cast<int64_t>(ion.len()) * (grow ? 1 : -1);
  // The total_allocated ftrace event reports the value before the
  // atomic_long_add / sub takes place.
  int64_t total_bytes = ion.total_allocated() + change_bytes;
  StringId global_name_id = ion_total_unknown_id_;
  StringId change_name_id = ion_change_unknown_id_;

  if (ion.has_heap_name()) {
    base::StringView heap_name = ion.heap_name();
    base::StackString<255> ion_name("mem.ion.%.*s", int(heap_name.size()),
                                    heap_name.data());
    global_name_id = context_->storage->InternString(ion_name.string_view());

    base::StackString<255> change_name("mem.ion_change.%.*s",
                                       int(heap_name.size()), heap_name.data());
    change_name_id = context_->storage->InternString(change_name.string_view());
  }

  // Push the global counter.
  TrackId track =
      context_->track_tracker->InternGlobalCounterTrack(global_name_id);
  context_->event_tracker->PushCounter(timestamp,
                                       static_cast<double>(total_bytes), track);

  // Push the change counter.
  // TODO(b/121331269): these should really be instant events.
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  track =
      context_->track_tracker->InternThreadCounterTrack(change_name_id, utid);
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(change_bytes), track);

  // We are reusing the same function for ion_heap_grow and ion_heap_shrink.
  // It is fine as the arguments are the same, but we need to be sure that the
  // protobuf field id for both are the same.
  static_assert(
      static_cast<int>(
          protos::pbzero::IonHeapGrowFtraceEvent::kTotalAllocatedFieldNumber) ==
              static_cast<int>(protos::pbzero::IonHeapShrinkFtraceEvent::
                                   kTotalAllocatedFieldNumber) &&
          static_cast<int>(
              protos::pbzero::IonHeapGrowFtraceEvent::kLenFieldNumber) ==
              static_cast<int>(
                  protos::pbzero::IonHeapShrinkFtraceEvent::kLenFieldNumber) &&
          static_cast<int>(
              protos::pbzero::IonHeapGrowFtraceEvent::kHeapNameFieldNumber) ==
              static_cast<int>(protos::pbzero::IonHeapShrinkFtraceEvent::
                                   kHeapNameFieldNumber),
      "ION field mismatch");
}

/** Parses ion heap events (introduced in 4.19 kernels). */
void FtraceParser::ParseIonStat(int64_t timestamp,
                                uint32_t pid,
                                protozero::ConstBytes data) {
  protos::pbzero::IonStatFtraceEvent::Decoder ion(data.data, data.size);
  // Push the global counter.
  TrackId track =
      context_->track_tracker->InternGlobalCounterTrack(ion_total_id_);
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(ion.total_allocated()), track);

  // Push the change counter.
  // TODO(b/121331269): these should really be instant events.
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  track =
      context_->track_tracker->InternThreadCounterTrack(ion_change_id_, utid);
  context_->event_tracker->PushCounter(timestamp,
                                       static_cast<double>(ion.len()), track);

  // Global track for individual buffer tracking
  auto async_track =
      context_->async_track_set_tracker->InternGlobalTrackSet(ion_buffer_id_);
  if (ion.len() > 0) {
    TrackId start_id =
        context_->async_track_set_tracker->Begin(async_track, ion.buffer_id());
    std::string buf = std::to_string(ion.len() / 1024) + " kB";
    context_->slice_tracker->Begin(
        timestamp, start_id, kNullStringId,
        context_->storage->InternString(base::StringView(buf)));
  } else {
    TrackId end_id =
        context_->async_track_set_tracker->End(async_track, ion.buffer_id());
    context_->slice_tracker->End(timestamp, end_id);
  }
}

void FtraceParser::ParseDmaHeapStat(int64_t timestamp,
                                    uint32_t pid,
                                    protozero::ConstBytes data) {
  protos::pbzero::DmaHeapStatFtraceEvent::Decoder dma_heap(data.data,
                                                           data.size);
  // Push the global counter.
  TrackId track =
      context_->track_tracker->InternGlobalCounterTrack(dma_heap_total_id_);
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(dma_heap.total_allocated()), track);

  // Push the change counter.
  // TODO(b/121331269): these should really be instant events.
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  track = context_->track_tracker->InternThreadCounterTrack(dma_heap_change_id_,
                                                            utid);
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(dma_heap.len()), track);

  // Global track for individual buffer tracking
  auto async_track =
      context_->async_track_set_tracker->InternGlobalTrackSet(dma_buffer_id_);
  if (dma_heap.len() > 0) {
    TrackId start_id = context_->async_track_set_tracker->Begin(
        async_track, static_cast<int64_t>(dma_heap.inode()));
    std::string buf = std::to_string(dma_heap.len() / 1024) + " kB";
    context_->slice_tracker->Begin(
        timestamp, start_id, kNullStringId,
        context_->storage->InternString(base::StringView(buf)));
  } else {
    TrackId end_id = context_->async_track_set_tracker->End(
        async_track, static_cast<int64_t>(dma_heap.inode()));
    context_->slice_tracker->End(timestamp, end_id);
  }
}

// This event has both the pid of the thread that sent the signal and the
// destination of the signal. Currently storing the pid of the destination.
void FtraceParser::ParseSignalGenerate(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::SignalGenerateFtraceEvent::Decoder sig(blob.data, blob.size);

  UniqueTid utid = context_->process_tracker->GetOrCreateThread(
      static_cast<uint32_t>(sig.pid()));
  int signal = sig.sig();
  TrackId track = context_->track_tracker->InternThreadTrack(utid);
  context_->slice_tracker->Scoped(
      timestamp, track, kNullStringId, signal_generate_id_, 0,
      [this, signal](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(signal_name_id_, Variadic::Integer(signal));
      });
}

void FtraceParser::ParseSignalDeliver(int64_t timestamp,
                                      uint32_t pid,
                                      ConstBytes blob) {
  protos::pbzero::SignalDeliverFtraceEvent::Decoder sig(blob.data, blob.size);
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  int signal = sig.sig();
  TrackId track = context_->track_tracker->InternThreadTrack(utid);
  context_->slice_tracker->Scoped(
      timestamp, track, kNullStringId, signal_deliver_id_, 0,
      [this, signal](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(signal_name_id_, Variadic::Integer(signal));
      });
}

void FtraceParser::ParseOOMScoreAdjUpdate(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::OomScoreAdjUpdateFtraceEvent::Decoder evt(blob.data,
                                                            blob.size);
  // The int16_t static cast is because older version of the on-device tracer
  // had a bug on negative varint encoding (b/120618641).
  int16_t oom_adj = static_cast<int16_t>(evt.oom_score_adj());
  uint32_t tid = static_cast<uint32_t>(evt.pid());
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(tid);
  context_->event_tracker->PushProcessCounterForThread(timestamp, oom_adj,
                                                       oom_score_adj_id_, utid);
}

void FtraceParser::ParseOOMKill(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::MarkVictimFtraceEvent::Decoder evt(blob.data, blob.size);
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(
      static_cast<uint32_t>(evt.pid()));
  TrackId track = context_->track_tracker->InternThreadTrack(utid);
  context_->slice_tracker->Scoped(timestamp, track, kNullStringId, oom_kill_id_,
                                  0);
}

void FtraceParser::ParseMmEventRecord(int64_t timestamp,
                                      uint32_t pid,
                                      ConstBytes blob) {
  protos::pbzero::MmEventRecordFtraceEvent::Decoder evt(blob.data, blob.size);
  uint32_t type = evt.type();
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);

  if (type >= mm_event_counter_names_.size()) {
    context_->storage->IncrementStats(stats::mm_unknown_type);
    return;
  }

  const auto& counter_names = mm_event_counter_names_[type];
  context_->event_tracker->PushProcessCounterForThread(
      timestamp, evt.count(), counter_names.count, utid);
  context_->event_tracker->PushProcessCounterForThread(
      timestamp, evt.max_lat(), counter_names.max_lat, utid);
  context_->event_tracker->PushProcessCounterForThread(
      timestamp, evt.avg_lat(), counter_names.avg_lat, utid);
}

void FtraceParser::ParseSysEvent(int64_t timestamp,
                                 uint32_t pid,
                                 bool is_enter,
                                 ConstBytes blob) {
  protos::pbzero::SysEnterFtraceEvent::Decoder evt(blob.data, blob.size);
  uint32_t syscall_num = static_cast<uint32_t>(evt.id());
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);

  SyscallTracker* syscall_tracker = SyscallTracker::GetOrCreate(context_);
  if (is_enter) {
    syscall_tracker->Enter(timestamp, utid, syscall_num);
  } else {
    syscall_tracker->Exit(timestamp, utid, syscall_num);
  }

  // We are reusing the same function for sys_enter and sys_exit.
  // It is fine as the arguments are the same, but we need to be sure that the
  // protobuf field id for both are the same.
  static_assert(
      static_cast<int>(protos::pbzero::SysEnterFtraceEvent::kIdFieldNumber) ==
          static_cast<int>(protos::pbzero::SysExitFtraceEvent::kIdFieldNumber),
      "field mismatch");
}

void FtraceParser::ParseTaskNewTask(int64_t timestamp,
                                    uint32_t source_tid,
                                    ConstBytes blob) {
  protos::pbzero::TaskNewtaskFtraceEvent::Decoder evt(blob.data, blob.size);
  uint32_t clone_flags = static_cast<uint32_t>(evt.clone_flags());
  uint32_t new_tid = static_cast<uint32_t>(evt.pid());
  StringId new_comm = context_->storage->InternString(evt.comm());
  auto* proc_tracker = context_->process_tracker.get();

  // task_newtask is raised both in the case of a new process creation (fork()
  // family) and thread creation (clone(CLONE_THREAD, ...)).
  static const uint32_t kCloneThread = 0x00010000;  // From kernel's sched.h.

  // If the process is a fork, start a new process except if the source tid is
  // kthreadd in which case just make it a new thread associated with
  // kthreadd.
  if ((clone_flags & kCloneThread) == 0 && source_tid != kKthreaddPid) {
    // This is a plain-old fork() or equivalent.
    proc_tracker->StartNewProcess(timestamp, source_tid, new_tid, new_comm,
                                  ThreadNamePriority::kFtrace);
    return;
  }

  if (source_tid == kKthreaddPid) {
    context_->process_tracker->SetProcessMetadata(
        kKthreaddPid, base::nullopt, kKthreaddName, base::StringView());
  }

  // This is a pthread_create or similar. Bind the two threads together, so
  // they get resolved to the same process.
  auto source_utid = proc_tracker->GetOrCreateThread(source_tid);
  auto new_utid = proc_tracker->StartNewThread(timestamp, new_tid);
  proc_tracker->UpdateThreadNameByUtid(new_utid, new_comm,
                                       ThreadNamePriority::kFtrace);
  proc_tracker->AssociateThreads(source_utid, new_utid);
}

void FtraceParser::ParseTaskRename(ConstBytes blob) {
  protos::pbzero::TaskRenameFtraceEvent::Decoder evt(blob.data, blob.size);
  uint32_t tid = static_cast<uint32_t>(evt.pid());
  StringId comm = context_->storage->InternString(evt.newcomm());
  context_->process_tracker->UpdateThreadNameAndMaybeProcessName(
      tid, comm, ThreadNamePriority::kFtrace);
}

void FtraceParser::ParseBinderTransaction(int64_t timestamp,
                                          uint32_t pid,
                                          ConstBytes blob) {
  protos::pbzero::BinderTransactionFtraceEvent::Decoder evt(blob.data,
                                                            blob.size);
  int32_t dest_node = static_cast<int32_t>(evt.target_node());
  uint32_t dest_tgid = static_cast<uint32_t>(evt.to_proc());
  uint32_t dest_tid = static_cast<uint32_t>(evt.to_thread());
  int32_t transaction_id = static_cast<int32_t>(evt.debug_id());
  bool is_reply = static_cast<int32_t>(evt.reply()) == 1;
  uint32_t flags = static_cast<uint32_t>(evt.flags());
  auto code_str = base::IntToHexString(evt.code()) + " Java Layer Dependent";
  StringId code = context_->storage->InternString(base::StringView(code_str));
  BinderTracker::GetOrCreate(context_)->Transaction(
      timestamp, pid, transaction_id, dest_node, dest_tgid, dest_tid, is_reply,
      flags, code);
}

void FtraceParser::ParseBinderTransactionReceived(int64_t timestamp,
                                                  uint32_t pid,
                                                  ConstBytes blob) {
  protos::pbzero::BinderTransactionReceivedFtraceEvent::Decoder evt(blob.data,
                                                                    blob.size);
  int32_t transaction_id = static_cast<int32_t>(evt.debug_id());
  BinderTracker::GetOrCreate(context_)->TransactionReceived(timestamp, pid,
                                                            transaction_id);
}

void FtraceParser::ParseBinderTransactionAllocBuf(int64_t timestamp,
                                                  uint32_t pid,
                                                  ConstBytes blob) {
  protos::pbzero::BinderTransactionAllocBufFtraceEvent::Decoder evt(blob.data,
                                                                    blob.size);
  uint64_t data_size = static_cast<uint64_t>(evt.data_size());
  uint64_t offsets_size = static_cast<uint64_t>(evt.offsets_size());

  BinderTracker::GetOrCreate(context_)->TransactionAllocBuf(
      timestamp, pid, data_size, offsets_size);
}

void FtraceParser::ParseBinderLocked(int64_t timestamp,
                                     uint32_t pid,
                                     ConstBytes blob) {
  protos::pbzero::BinderLockedFtraceEvent::Decoder evt(blob.data, blob.size);
  BinderTracker::GetOrCreate(context_)->Locked(timestamp, pid);
}

void FtraceParser::ParseBinderLock(int64_t timestamp,
                                   uint32_t pid,
                                   ConstBytes blob) {
  protos::pbzero::BinderLockFtraceEvent::Decoder evt(blob.data, blob.size);
  BinderTracker::GetOrCreate(context_)->Lock(timestamp, pid);
}

void FtraceParser::ParseBinderUnlock(int64_t timestamp,
                                     uint32_t pid,
                                     ConstBytes blob) {
  protos::pbzero::BinderUnlockFtraceEvent::Decoder evt(blob.data, blob.size);
  BinderTracker::GetOrCreate(context_)->Unlock(timestamp, pid);
}

void FtraceParser::ParseClockSetRate(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::ClockSetRateFtraceEvent::Decoder evt(blob.data, blob.size);
  static const char kSubtitle[] = "Frequency";
  ClockRate(timestamp, evt.name(), kSubtitle, evt.state());
}

void FtraceParser::ParseClockEnable(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::ClockEnableFtraceEvent::Decoder evt(blob.data, blob.size);
  static const char kSubtitle[] = "State";
  ClockRate(timestamp, evt.name(), kSubtitle, evt.state());
}

void FtraceParser::ParseClockDisable(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::ClockDisableFtraceEvent::Decoder evt(blob.data, blob.size);
  static const char kSubtitle[] = "State";
  ClockRate(timestamp, evt.name(), kSubtitle, evt.state());
}

void FtraceParser::ClockRate(int64_t timestamp,
                             base::StringView clock_name,
                             base::StringView subtitle,
                             uint64_t rate) {
  base::StackString<255> counter_name("%.*s %.*s", int(clock_name.size()),
                                      clock_name.data(), int(subtitle.size()),
                                      subtitle.data());
  StringId name = context_->storage->InternString(counter_name.c_str());
  TrackId track = context_->track_tracker->InternGlobalCounterTrack(name);
  context_->event_tracker->PushCounter(timestamp, static_cast<double>(rate),
                                       track);
}

void FtraceParser::ParseScmCallStart(int64_t timestamp,
                                     uint32_t pid,
                                     ConstBytes blob) {
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
  protos::pbzero::ScmCallStartFtraceEvent::Decoder evt(blob.data, blob.size);

  char str[64];
  sprintf(str, "scm id=%#" PRIx64, evt.x0());
  StringId name_id = context_->storage->InternString(str);
  context_->slice_tracker->Begin(timestamp, track_id, kNullStringId, name_id);
}

void FtraceParser::ParseScmCallEnd(int64_t timestamp,
                                   uint32_t pid,
                                   ConstBytes blob) {
  protos::pbzero::ScmCallEndFtraceEvent::Decoder evt(blob.data, blob.size);
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
  context_->slice_tracker->End(timestamp, track_id);
}

void FtraceParser::ParseDirectReclaimBegin(int64_t timestamp,
                                           uint32_t pid,
                                           ConstBytes blob) {
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
  protos::pbzero::MmVmscanDirectReclaimBeginFtraceEvent::Decoder
      direct_reclaim_begin(blob.data, blob.size);

  StringId name_id =
      context_->storage->InternString("mm_vmscan_direct_reclaim");

  auto args_inserter = [this, &direct_reclaim_begin](
                           ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(direct_reclaim_order_id_,
                     Variadic::Integer(direct_reclaim_begin.order()));
    inserter->AddArg(direct_reclaim_may_writepage_id_,
                     Variadic::Integer(direct_reclaim_begin.may_writepage()));
    inserter->AddArg(
        direct_reclaim_gfp_flags_id_,
        Variadic::UnsignedInteger(direct_reclaim_begin.gfp_flags()));
  };
  context_->slice_tracker->Begin(timestamp, track_id, kNullStringId, name_id,
                                 args_inserter);
}

void FtraceParser::ParseDirectReclaimEnd(int64_t timestamp,
                                         uint32_t pid,
                                         ConstBytes blob) {
  protos::pbzero::ScmCallEndFtraceEvent::Decoder evt(blob.data, blob.size);
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
  protos::pbzero::MmVmscanDirectReclaimEndFtraceEvent::Decoder
      direct_reclaim_end(blob.data, blob.size);

  auto args_inserter =
      [this, &direct_reclaim_end](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(
            direct_reclaim_nr_reclaimed_id_,
            Variadic::UnsignedInteger(direct_reclaim_end.nr_reclaimed()));
      };
  context_->slice_tracker->End(timestamp, track_id, kNullStringId,
                               kNullStringId, args_inserter);
}

void FtraceParser::ParseWorkqueueExecuteStart(
    uint32_t cpu,
    int64_t timestamp,
    uint32_t pid,
    ConstBytes blob,
    PacketSequenceStateGeneration* seq_state) {
  protos::pbzero::WorkqueueExecuteStartFtraceEvent::Decoder evt(blob.data,
                                                                blob.size);

  auto* interned_string = seq_state->LookupInternedMessage<
      protos::pbzero::InternedData::kKernelSymbolsFieldNumber,
      protos::pbzero::InternedString>(static_cast<uint32_t>(evt.function()));
  StringId name_id;
  if (interned_string) {
    protozero::ConstBytes str = interned_string->str();
    name_id = context_->storage->InternString(
        base::StringView(reinterpret_cast<const char*>(str.data), str.size));
  } else {
    base::StackString<255> slice_name("%#" PRIx64, evt.function());
    name_id = context_->storage->InternString(slice_name.string_view());
  }

  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track = context_->track_tracker->InternThreadTrack(utid);

  auto args_inserter = [this, cpu](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(cpu_id_, Variadic::Integer(cpu));
  };
  context_->slice_tracker->Begin(timestamp, track, workqueue_id_, name_id,
                                 args_inserter);
}

void FtraceParser::ParseWorkqueueExecuteEnd(int64_t timestamp,
                                            uint32_t pid,
                                            ConstBytes blob) {
  protos::pbzero::WorkqueueExecuteEndFtraceEvent::Decoder evt(blob.data,
                                                              blob.size);
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track = context_->track_tracker->InternThreadTrack(utid);
  context_->slice_tracker->End(timestamp, track, workqueue_id_);
}

void FtraceParser::ParseIrqHandlerEntry(uint32_t cpu,
                                        int64_t timestamp,
                                        protozero::ConstBytes blob) {
  protos::pbzero::IrqHandlerEntryFtraceEvent::Decoder evt(blob.data, blob.size);
  base::StackString<255> track_name("Irq Cpu %d", cpu);
  StringId track_name_id =
      context_->storage->InternString(track_name.string_view());

  base::StringView irq_name = evt.name();
  base::StackString<255> slice_name("IRQ (%.*s)", int(irq_name.size()),
                                    irq_name.data());
  StringId slice_name_id =
      context_->storage->InternString(slice_name.string_view());
  TrackId track = context_->track_tracker->InternCpuTrack(track_name_id, cpu);
  context_->slice_tracker->Begin(timestamp, track, irq_id_, slice_name_id);
}

void FtraceParser::ParseIrqHandlerExit(uint32_t cpu,
                                       int64_t timestamp,
                                       protozero::ConstBytes blob) {
  protos::pbzero::IrqHandlerExitFtraceEvent::Decoder evt(blob.data, blob.size);
  base::StackString<255> track_name("Irq Cpu %d", cpu);
  StringId track_name_id =
      context_->storage->InternString(track_name.string_view());
  TrackId track = context_->track_tracker->InternCpuTrack(track_name_id, cpu);

  base::StackString<255> status("%s", evt.ret() == 1 ? "handled" : "unhandled");
  StringId status_id = context_->storage->InternString(status.string_view());
  auto args_inserter = [this,
                        &status_id](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(ret_arg_id_, Variadic::String(status_id));
  };
  context_->slice_tracker->End(timestamp, track, irq_id_, {}, args_inserter);
}

void FtraceParser::ParseSoftIrqEntry(uint32_t cpu,
                                     int64_t timestamp,
                                     protozero::ConstBytes blob) {
  protos::pbzero::SoftirqEntryFtraceEvent::Decoder evt(blob.data, blob.size);
  base::StackString<255> track_name("SoftIrq Cpu %d", cpu);
  StringId track_name_id =
      context_->storage->InternString(track_name.string_view());
  auto num_actions = sizeof(kActionNames) / sizeof(*kActionNames);
  if (evt.vec() >= num_actions) {
    PERFETTO_DFATAL("No action name at index %d for softirq event.", evt.vec());
    return;
  }
  base::StringView slice_name = kActionNames[evt.vec()];
  StringId slice_name_id = context_->storage->InternString(slice_name);
  TrackId track = context_->track_tracker->InternCpuTrack(track_name_id, cpu);
  context_->slice_tracker->Begin(timestamp, track, irq_id_, slice_name_id);
}

void FtraceParser::ParseSoftIrqExit(uint32_t cpu,
                                    int64_t timestamp,
                                    protozero::ConstBytes blob) {
  protos::pbzero::SoftirqExitFtraceEvent::Decoder evt(blob.data, blob.size);
  base::StackString<255> track_name("SoftIrq Cpu %d", cpu);
  StringId track_name_id =
      context_->storage->InternString(track_name.string_view());
  TrackId track = context_->track_tracker->InternCpuTrack(track_name_id, cpu);
  auto vec = evt.vec();
  auto args_inserter = [this, vec](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(vec_arg_id_, Variadic::Integer(vec));
  };
  context_->slice_tracker->End(timestamp, track, irq_id_, {}, args_inserter);
}

void FtraceParser::ParseGpuMemTotal(int64_t timestamp,
                                    protozero::ConstBytes data) {
  protos::pbzero::GpuMemTotalFtraceEvent::Decoder gpu_mem_total(data.data,
                                                                data.size);

  TrackId track = kInvalidTrackId;
  const uint32_t pid = gpu_mem_total.pid();
  if (pid == 0) {
    // Pid 0 is used to indicate the global total
    track = context_->track_tracker->InternGlobalCounterTrack(
        gpu_mem_total_name_id_, gpu_mem_total_unit_id_,
        gpu_mem_total_global_desc_id_);
  } else {
    // It's possible for GpuMemTotal ftrace events to be emitted by kworker
    // threads *after* process death. In this case, we simply want to discard
    // the event as otherwise we would create fake processes which we
    // definitely want to avoid.
    // See b/192274404 for more info.
    base::Optional<UniqueTid> opt_utid =
        context_->process_tracker->GetThreadOrNull(pid);
    if (!opt_utid)
      return;

    // If the thread does exist, the |pid| in gpu_mem_total events is always a
    // true process id (and not a thread id) so ensure there is an association
    // between the tid and pid.
    UniqueTid updated_utid = context_->process_tracker->UpdateThread(pid, pid);
    PERFETTO_DCHECK(updated_utid == *opt_utid);

    // UpdateThread above should ensure this is always set.
    UniquePid upid = *context_->storage->thread_table().upid()[*opt_utid];
    PERFETTO_DCHECK(context_->storage->process_table().pid()[upid] == pid);

    track = context_->track_tracker->InternProcessCounterTrack(
        gpu_mem_total_name_id_, upid, gpu_mem_total_unit_id_,
        gpu_mem_total_proc_desc_id_);
  }
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(gpu_mem_total.size()), track);
}

void FtraceParser::ParseThermalTemperature(int64_t timestamp,
                                           protozero::ConstBytes blob) {
  protos::pbzero::ThermalTemperatureFtraceEvent::Decoder evt(blob.data,
                                                             blob.size);
  base::StringView thermal_zone = evt.thermal_zone();
  base::StackString<255> counter_name(
      "%.*s Temperature", int(thermal_zone.size()), thermal_zone.data());
  StringId name = context_->storage->InternString(counter_name.string_view());
  TrackId track = context_->track_tracker->InternGlobalCounterTrack(name);
  context_->event_tracker->PushCounter(timestamp, evt.temp(), track);
}

void FtraceParser::ParseCdevUpdate(int64_t timestamp,
                                   protozero::ConstBytes blob) {
  protos::pbzero::CdevUpdateFtraceEvent::Decoder evt(blob.data, blob.size);
  base::StringView type = evt.type();
  base::StackString<255> counter_name("%.*s Cooling Device", int(type.size()),
                                      type.data());
  StringId name = context_->storage->InternString(counter_name.string_view());
  TrackId track = context_->track_tracker->InternGlobalCounterTrack(name);
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(evt.target()), track);
}

void FtraceParser::ParseSchedBlockedReason(
    int64_t timestamp,
    protozero::ConstBytes blob,
    PacketSequenceStateGeneration* seq_state) {
  protos::pbzero::SchedBlockedReasonFtraceEvent::Decoder evt(blob);
  uint32_t pid = static_cast<uint32_t>(evt.pid());
  auto utid = context_->process_tracker->GetOrCreateThread(pid);
  auto io_wait = evt.io_wait();
  uint32_t caller_iid = static_cast<uint32_t>(evt.caller());
  auto* interned_string = seq_state->LookupInternedMessage<
      protos::pbzero::InternedData::kKernelSymbolsFieldNumber,
      protos::pbzero::InternedString>(caller_iid);

  if (interned_string) {
    protozero::ConstBytes str = interned_string->str();
    StringId str_id = context_->storage->InternString(
        base::StringView(reinterpret_cast<const char*>(str.data), str.size));
    auto instant_id = context_->storage->mutable_legacy_instant_table()->Insert(
        {timestamp, sched_blocked_reason_id_, utid});
    context_->args_tracker->AddArgsTo(instant_id.id)
        .AddArg(io_wait_id_, Variadic::Boolean(io_wait))
        .AddArg(function_id_, Variadic::String(str_id));
    return;
  }
  auto instant_id = context_->storage->mutable_legacy_instant_table()->Insert(
      {timestamp, sched_blocked_reason_id_, utid});
  context_->args_tracker->AddArgsTo(instant_id.id)
      .AddArg(io_wait_id_, Variadic::Boolean(io_wait));
}

void FtraceParser::ParseFastRpcDmaStat(int64_t timestamp,
                                       uint32_t pid,
                                       protozero::ConstBytes blob) {
  protos::pbzero::FastrpcDmaStatFtraceEvent::Decoder evt(blob.data, blob.size);

  StringId name;
  if (0 <= evt.cid() && evt.cid() < static_cast<int32_t>(kFastRpcCounterSize)) {
    name = fast_rpc_delta_names_[static_cast<size_t>(evt.cid())];
  } else {
    base::StackString<64> str("mem.fastrpc[%" PRId32 "]", evt.cid());
    name = context_->storage->InternString(str.string_view());
  }

  StringId total_name;
  if (0 <= evt.cid() && evt.cid() < static_cast<int32_t>(kFastRpcCounterSize)) {
    total_name = fast_rpc_total_names_[static_cast<size_t>(evt.cid())];
  } else {
    base::StackString<64> str("mem.fastrpc[%" PRId32 "]", evt.cid());
    total_name = context_->storage->InternString(str.string_view());
  }

  // Push the global counter.
  TrackId track = context_->track_tracker->InternGlobalCounterTrack(total_name);
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(evt.total_allocated()), track);

  // Push the change counter.
  // TODO(b/121331269): these should really be instant events.
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId delta_track =
      context_->track_tracker->InternThreadCounterTrack(name, utid);
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(evt.len()), delta_track);
}

void FtraceParser::ParseCpuhpPause(int64_t,
                                   uint32_t,
                                   protozero::ConstBytes blob) {
  protos::pbzero::CpuhpPauseFtraceEvent::Decoder evt(blob.data, blob.size);
  // TODO(b/183110813): Parse and visualize this event.
}

void FtraceParser::ParseNetifReceiveSkb(uint32_t cpu,
                                        int64_t timestamp,
                                        protozero::ConstBytes blob) {
  protos::pbzero::NetifReceiveSkbFtraceEvent::Decoder evt(blob.data, blob.size);
  base::StringView net_device = evt.name();
  base::StackString<255> counter_name("%.*s Received KB",
                                      static_cast<int>(net_device.size()),
                                      net_device.data());
  StringId name = context_->storage->InternString(counter_name.string_view());

  nic_received_bytes_[name] += evt.len();

  uint64_t nic_received_kilobytes = nic_received_bytes_[name] / 1024;
  TrackId track = context_->track_tracker->InternGlobalCounterTrack(name);
  base::Optional<CounterId> id = context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(nic_received_kilobytes), track);
  if (!id) {
    return;
  }
  // Store cpu & len as args for metrics computation
  StringId cpu_key = context_->storage->InternString("cpu");
  StringId len_key = context_->storage->InternString("len");
  context_->args_tracker->AddArgsTo(*id)
      .AddArg(cpu_key, Variadic::UnsignedInteger(cpu))
      .AddArg(len_key, Variadic::UnsignedInteger(evt.len()));
}

void FtraceParser::ParseNetDevXmit(uint32_t cpu,
                                   int64_t timestamp,
                                   protozero::ConstBytes blob) {
  protos::pbzero::NetDevXmitFtraceEvent::Decoder evt(blob.data, blob.size);
  base::StringView net_device = evt.name();
  base::StackString<255> counter_name("%.*s Transmitted KB",
                                      static_cast<int>(net_device.size()),
                                      net_device.data());
  StringId name = context_->storage->InternString(counter_name.string_view());

  // Make sure driver took care of packet.
  if (evt.rc() != 0) {
    return;
  }
  nic_transmitted_bytes_[name] += evt.len();

  uint64_t nic_transmitted_kilobytes = nic_transmitted_bytes_[name] / 1024;
  TrackId track = context_->track_tracker->InternGlobalCounterTrack(name);
  base::Optional<CounterId> id = context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(nic_transmitted_kilobytes), track);
  if (!id) {
    return;
  }
  // Store cpu & len as args for metrics computation.
  context_->args_tracker->AddArgsTo(*id)
      .AddArg(cpu_id_, Variadic::UnsignedInteger(cpu))
      .AddArg(len_arg_id_, Variadic::UnsignedInteger(evt.len()));
}

void FtraceParser::ParseInetSockSetState(int64_t timestamp,
                                         uint32_t pid,
                                         protozero::ConstBytes blob) {
  protos::pbzero::InetSockSetStateFtraceEvent::Decoder evt(blob.data,
                                                           blob.size);

  // Skip non TCP protocol.
  if (evt.protocol() != kIpprotoTcp) {
    PERFETTO_ELOG("skip non tcp protocol");
    return;
  }

  // Skip non IP protocol.
  if (evt.family() != kAfNet && evt.family() != kAfNet6) {
    PERFETTO_ELOG("skip non IP protocol");
    return;
  }

  // Skip invalid TCP state.
  if (evt.newstate() >= TCP_MAX_STATES || evt.oldstate() >= TCP_MAX_STATES) {
    PERFETTO_ELOG("skip invalid tcp state");
    return;
  }

  auto got = skaddr_to_stream_.find(evt.skaddr());
  if (got == skaddr_to_stream_.end()) {
    skaddr_to_stream_[evt.skaddr()] = ++num_of_tcp_stream_;
  }
  uint32_t stream = skaddr_to_stream_[evt.skaddr()];
  char stream_str[64];
  sprintf(stream_str, "TCP stream#%" PRIu32 "", stream);
  StringId stream_id = context_->storage->InternString(stream_str);

  StringId slice_name_id;
  if (evt.newstate() == TCP_SYN_SENT) {
    base::StackString<32> str("%s(pid=%" PRIu32 ")",
                              kTcpStateNames[evt.newstate()], pid);
    slice_name_id = context_->storage->InternString(str.string_view());
  } else if (evt.newstate() == TCP_ESTABLISHED) {
    base::StackString<64> str("%s(sport=%" PRIu32 ",dport=%" PRIu32 ")",
                              kTcpStateNames[evt.newstate()], evt.sport(),
                              evt.dport());
    slice_name_id = context_->storage->InternString(str.string_view());
  } else {
    base::StringView slice_name = kTcpStateNames[evt.newstate()];
    slice_name_id = context_->storage->InternString(slice_name);
  }

  // Push to async task set tracker.
  auto async_track =
      context_->async_track_set_tracker->InternGlobalTrackSet(stream_id);
  TrackId end_id = context_->async_track_set_tracker->End(
      async_track, static_cast<int64_t>(evt.skaddr()));
  context_->slice_tracker->End(timestamp, end_id);
  TrackId start_id = context_->async_track_set_tracker->Begin(
      async_track, static_cast<int64_t>(evt.skaddr()));
  context_->slice_tracker->Begin(timestamp, start_id, tcp_state_id_,
                                 slice_name_id);
}

void FtraceParser::ParseTcpRetransmitSkb(int64_t timestamp,
                                         protozero::ConstBytes blob) {
  protos::pbzero::TcpRetransmitSkbFtraceEvent::Decoder evt(blob.data,
                                                           blob.size);

  // Push event as instant to async task set tracker.
  auto async_track = context_->async_track_set_tracker->InternGlobalTrackSet(
      tcp_retransmited_name_id_);
  base::StackString<64> str("sport=%" PRIu32 ",dport=%" PRIu32 "", evt.sport(),
                            evt.dport());
  StringId slice_name_id = context_->storage->InternString(str.string_view());
  TrackId track_id =
      context_->async_track_set_tracker->Scoped(async_track, timestamp, 0);
  context_->slice_tracker->Scoped(timestamp, track_id, tcp_event_id_,
                                  slice_name_id, 0);
}

void FtraceParser::ParseNapiGroReceiveEntry(uint32_t cpu,
                                            int64_t timestamp,
                                            protozero::ConstBytes blob) {
  protos::pbzero::NapiGroReceiveEntryFtraceEvent::Decoder evt(blob.data,
                                                              blob.size);
  base::StackString<255> track_name("Napi Gro Cpu %d", cpu);
  StringId track_name_id =
      context_->storage->InternString(track_name.string_view());
  base::StringView net_device = evt.name();
  StringId slice_name_id = context_->storage->InternString(net_device);
  TrackId track = context_->track_tracker->InternCpuTrack(track_name_id, cpu);
  auto len = evt.len();
  auto args_inserter = [this, len](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(len_arg_id_, Variadic::Integer(len));
  };
  context_->slice_tracker->Begin(timestamp, track, napi_gro_id_, slice_name_id,
                                 args_inserter);
}

void FtraceParser::ParseNapiGroReceiveExit(uint32_t cpu,
                                           int64_t timestamp,
                                           protozero::ConstBytes blob) {
  protos::pbzero::NapiGroReceiveExitFtraceEvent::Decoder evt(blob.data,
                                                             blob.size);
  base::StackString<255> track_name("Napi Gro Cpu %d", cpu);
  StringId track_name_id =
      context_->storage->InternString(track_name.string_view());
  TrackId track = context_->track_tracker->InternCpuTrack(track_name_id, cpu);
  auto ret = evt.ret();
  auto args_inserter = [this, ret](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(ret_arg_id_, Variadic::Integer(ret));
  };
  context_->slice_tracker->End(timestamp, track, napi_gro_id_, {},
                               args_inserter);
}

void FtraceParser::ParseCpuFrequencyLimits(int64_t timestamp,
                                           protozero::ConstBytes blob) {
  protos::pbzero::CpuFrequencyLimitsFtraceEvent::Decoder evt(blob.data,
                                                             blob.size);
  base::StackString<255> max_counter_name("Cpu %" PRIu32 " Max Freq Limit",
                                          evt.cpu_id());
  base::StackString<255> min_counter_name("Cpu %" PRIu32 " Min Freq Limit",
                                          evt.cpu_id());
  // Push max freq to global counter.
  StringId max_name = context_->storage->InternString(max_counter_name.c_str());
  TrackId max_track =
      context_->track_tracker->InternGlobalCounterTrack(max_name);
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(evt.max_freq()), max_track);

  // Push min freq to global counter.
  StringId min_name = context_->storage->InternString(min_counter_name.c_str());
  TrackId min_track =
      context_->track_tracker->InternGlobalCounterTrack(min_name);
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(evt.min_freq()), min_track);
}

void FtraceParser::ParseKfreeSkb(int64_t timestamp,
                                 protozero::ConstBytes blob) {
  protos::pbzero::KfreeSkbFtraceEvent::Decoder evt(blob.data, blob.size);

  // Skip non IP & IPV6 protocol.
  if (evt.protocol() != kEthPIp && evt.protocol() != kEthPIp6) {
    return;
  }
  num_of_kfree_skb_ip_prot += 1;

  TrackId track =
      context_->track_tracker->InternGlobalCounterTrack(kfree_skb_name_id_);
  base::Optional<CounterId> id = context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(num_of_kfree_skb_ip_prot), track);
  if (!id) {
    return;
  }
  base::StackString<255> prot("%s", evt.protocol() == kEthPIp ? "IP" : "IPV6");
  StringId prot_id = context_->storage->InternString(prot.string_view());
  // Store protocol as args for metrics computation.
  context_->args_tracker->AddArgsTo(*id).AddArg(protocol_arg_id_,
                                                Variadic::String(prot_id));
}

void FtraceParser::ParseCrosEcSensorhubData(int64_t timestamp,
                                            protozero::ConstBytes blob) {
  protos::pbzero::CrosEcSensorhubDataFtraceEvent::Decoder evt(blob.data,
                                                              blob.size);

  // Push the global counter.
  TrackId track = context_->track_tracker->InternGlobalCounterTrack(
      context_->storage->InternString(
          base::StringView("cros_ec.cros_ec_sensorhub_data." +
                           std::to_string(evt.ec_sensor_num()))));

  auto args_inserter = [this, &evt](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(cros_ec_arg_num_id_,
                     Variadic::Integer(evt.ec_sensor_num()));
    inserter->AddArg(
        cros_ec_arg_ec_id_,
        Variadic::Integer(evt.fifo_timestamp() - evt.current_timestamp()));
    inserter->AddArg(cros_ec_arg_sample_ts_id_,
                     Variadic::Integer(evt.current_timestamp()));
  };

  context_->event_tracker->PushCounter(
      timestamp,
      static_cast<double>(evt.current_time() - evt.current_timestamp()), track,
      args_inserter);
}

void FtraceParser::ParseUfshcdClkGating(int64_t timestamp,
                                        protozero::ConstBytes blob) {
  protos::pbzero::UfshcdClkGatingFtraceEvent::Decoder evt(blob.data, blob.size);
  int32_t clk_state = 0;

  switch (evt.state()) {
    case 1:
      // Change ON state to 3
      clk_state = 3;
      break;
    case 2:
      // Change REQ_OFF state to 1
      clk_state = 1;
      break;
    case 3:
      // Change REQ_ON state to 2
      clk_state = 2;
      break;
  }
  TrackId track =
      context_->track_tracker->InternGlobalCounterTrack(ufs_clkgating_id_);
  context_->event_tracker->PushCounter(timestamp,
                                       static_cast<double>(clk_state), track);
}

void FtraceParser::ParseUfshcdCommand(int64_t timestamp,
                                      protozero::ConstBytes blob) {
  protos::pbzero::UfshcdCommandFtraceEvent::Decoder evt(blob.data, blob.size);
  uint32_t num = evt.doorbell() > 0
                     ? static_cast<uint32_t>(PERFETTO_POPCOUNT(evt.doorbell()))
                     : (evt.str_t() == 1 ? 0 : 1);

  TrackId track =
      context_->track_tracker->InternGlobalCounterTrack(ufs_command_count_id_);
  context_->event_tracker->PushCounter(timestamp, static_cast<double>(num),
                                       track);
}

void FtraceParser::ParseWakeSourceActivate(int64_t timestamp,
                                           protozero::ConstBytes blob) {
  protos::pbzero::WakeupSourceActivateFtraceEvent::Decoder evt(blob.data,
                                                               blob.size);
  std::string event_name = evt.name().ToStdString();

  uint32_t count = active_wakelock_to_count_[event_name];

  active_wakelock_to_count_[event_name] += 1;

  // There is already an active track with this name, don't create another.
  if (count > 0) {
    return;
  }

  base::StackString<32> str("Wakelock(%s)", event_name.c_str());
  StringId stream_id = context_->storage->InternString(str.string_view());

  auto async_track =
      context_->async_track_set_tracker->InternGlobalTrackSet(stream_id);

  TrackId start_id = context_->async_track_set_tracker->Begin(async_track, 0);

  context_->slice_tracker->Begin(timestamp, start_id, kNullStringId, stream_id);
}

void FtraceParser::ParseWakeSourceDeactivate(int64_t timestamp,
                                             protozero::ConstBytes blob) {
  protos::pbzero::WakeupSourceDeactivateFtraceEvent::Decoder evt(blob.data,
                                                                 blob.size);

  std::string event_name = evt.name().ToStdString();
  uint32_t count = active_wakelock_to_count_[event_name];
  active_wakelock_to_count_[event_name] = count > 0 ? count - 1 : 0;
  if (count != 1) {
    return;
  }

  base::StackString<32> str("Wakelock(%s)", event_name.c_str());
  StringId stream_id = context_->storage->InternString(str.string_view());
  auto async_track =
      context_->async_track_set_tracker->InternGlobalTrackSet(stream_id);

  TrackId end_id = context_->async_track_set_tracker->End(async_track, 0);
  context_->slice_tracker->End(timestamp, end_id);
}

void FtraceParser::ParseSuspendResume(int64_t timestamp,
                                      protozero::ConstBytes blob) {
  protos::pbzero::SuspendResumeFtraceEvent::Decoder evt(blob.data, blob.size);

  auto async_track = context_->async_track_set_tracker->InternGlobalTrackSet(
      suspend_resume_name_id_);

  base::StackString<64> str("%s(%" PRIu32 ")",
                            evt.action().ToStdString().c_str(), evt.val());
  StringId slice_name_id = context_->storage->InternString(str.string_view());

  if (evt.start()) {
    TrackId start_id = context_->async_track_set_tracker->Begin(
        async_track, static_cast<int64_t>(evt.val()));
    context_->slice_tracker->Begin(timestamp, start_id, suspend_resume_name_id_,
                                   slice_name_id);
  } else {
    TrackId end_id = context_->async_track_set_tracker->End(
        async_track, static_cast<int64_t>(evt.val()));
    context_->slice_tracker->End(timestamp, end_id);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
