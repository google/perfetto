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
#include "src/trace_processor/importers/syscalls/syscall_tracker.h"
#include "src/trace_processor/importers/systrace/systrace_parser.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/softirq_action.h"

#include "protos/perfetto/common/gpu_counter_descriptor.pbzero.h"
#include "protos/perfetto/trace/ftrace/binder.pbzero.h"
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
#include "protos/perfetto/trace/ftrace/mm_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/oom.pbzero.h"
#include "protos/perfetto/trace/ftrace/power.pbzero.h"
#include "protos/perfetto/trace/ftrace/raw_syscalls.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/ftrace/scm.pbzero.h"
#include "protos/perfetto/trace/ftrace/sde.pbzero.h"
#include "protos/perfetto/trace/ftrace/signal.pbzero.h"
#include "protos/perfetto/trace/ftrace/systrace.pbzero.h"
#include "protos/perfetto/trace/ftrace/task.pbzero.h"
#include "protos/perfetto/trace/ftrace/thermal.pbzero.h"
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
constexpr auto kKernelFunctionFields =
    std::array<FtraceEventAndFieldId, 1>{{FtraceEventAndFieldId{
        protos::pbzero::FtraceEvent::kSchedBlockedReasonFieldNumber,
        protos::pbzero::SchedBlockedReasonFtraceEvent::kCallerFieldNumber}}};

}  // namespace

FtraceParser::FtraceParser(TraceProcessorContext* context)
    : context_(context),
      rss_stat_tracker_(context),
      sched_wakeup_name_id_(context->storage->InternString("sched_wakeup")),
      sched_waking_name_id_(context->storage->InternString("sched_waking")),
      cpu_freq_name_id_(context->storage->InternString("cpufreq")),
      gpu_freq_name_id_(context->storage->InternString("gpufreq")),
      cpu_idle_name_id_(context->storage->InternString("cpuidle")),
      ion_total_id_(context->storage->InternString("mem.ion")),
      ion_change_id_(context->storage->InternString("mem.ion_change")),
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
      ret_arg_id_(context_->storage->InternString("ret")),
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
      function_id_(context->storage->InternString("function")) {
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
  fast_rpc_counter_names_[0] = context->storage->InternString("mem.fastrpc[ASDP]");
  fast_rpc_counter_names_[1] = context->storage->InternString("mem.fastrpc[MDSP]");
  fast_rpc_counter_names_[2] = context->storage->InternString("mem.fastrpc[SDSP]");
  fast_rpc_counter_names_[3] = context->storage->InternString("mem.fastrpc[CDSP]");

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
  size_t phase =
      evt.phase() == protos::pbzero::FtraceStats_Phase_END_OF_TRACE ? 1 : 0;

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
    storage->SetIndexedStats(stats::ftrace_cpu_entries_begin + phase, cpu,
                             static_cast<int64_t>(cpu_stats.entries()));
    storage->SetIndexedStats(stats::ftrace_cpu_overrun_begin + phase, cpu,
                             static_cast<int64_t>(cpu_stats.overrun()));
    storage->SetIndexedStats(stats::ftrace_cpu_commit_overrun_begin + phase,
                             cpu,
                             static_cast<int64_t>(cpu_stats.commit_overrun()));
    storage->SetIndexedStats(stats::ftrace_cpu_bytes_read_begin + phase, cpu,
                             static_cast<int64_t>(cpu_stats.bytes_read()));

    // oldest_event_ts can often be set to very high values, possibly because
    // of wrapping. Ensure that we are not overflowing to avoid ubsan
    // complaining.
    double oldest_event_ts = cpu_stats.oldest_event_ts() * 1e9;
    // NB: This comparison is correct only because of the >=, it would be
    // incorrect with >. std::numeric_limits<int64_t>::max() converted to
    // a double is the next value representable as a double that is *larger*
    // than std::numeric_limits<int64_t>::max(). All values that are
    // representable as doubles and < than that value are thus representable as
    // int64_t.
    if (oldest_event_ts >=
        static_cast<double>(std::numeric_limits<int64_t>::max())) {
      storage->SetIndexedStats(stats::ftrace_cpu_oldest_event_ts_begin + phase,
                               cpu, std::numeric_limits<int64_t>::max());
    } else {
      storage->SetIndexedStats(stats::ftrace_cpu_oldest_event_ts_begin + phase,
                               cpu, static_cast<int64_t>(oldest_event_ts));
    }

    storage->SetIndexedStats(stats::ftrace_cpu_now_ts_begin + phase, cpu,
                             static_cast<int64_t>(cpu_stats.now_ts() * 1e9));
    storage->SetIndexedStats(stats::ftrace_cpu_dropped_events_begin + phase,
                             cpu,
                             static_cast<int64_t>(cpu_stats.dropped_events()));
    storage->SetIndexedStats(stats::ftrace_cpu_read_events_begin + phase, cpu,
                             static_cast<int64_t>(cpu_stats.read_events()));
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
  PacketSequenceStateGeneration* seq_state = ttp.ftrace_event.sequence_state;
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
        ParseSchedWakeup(ts, data);
        break;
      }
      case FtraceEvent::kSchedWakingFieldNumber: {
        ParseSchedWaking(ts, data);
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
      case FtraceEvent::kRssStatFieldNumber: {
        rss_stat_tracker_.ParseRssStat(ts, pid, data);
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
      case FtraceEvent::kSignalGenerateFieldNumber: {
        ParseSignalGenerate(ts, data);
        break;
      }
      case FtraceEvent::kSignalDeliverFieldNumber: {
        ParseSignalDeliver(ts, pid, data);
        break;
      }
      case FtraceEvent::kLowmemoryKillFieldNumber: {
        ParseLowmemoryKill(ts, data);
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
      case FtraceEvent::kWorkqueueExecuteStartFieldNumber: {
        ParseWorkqueueExecuteStart(ts, pid, data);
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
    int64_t ts,
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
  RawId id = context_->storage->mutable_raw_table()
                 ->Insert({ts, message_strings.message_name_id, cpu, utid})
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

      // If we don't have the string for this field (can happen if symbolization
      // wasn't enabled, if reading the symbols errored out or on legacy traces)
      // then just add the field as a normal arg.
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
void FtraceParser::ParseSchedSwitch(uint32_t cpu, int64_t ts, ConstBytes blob) {
  protos::pbzero::SchedSwitchFtraceEvent::Decoder ss(blob.data, blob.size);
  uint32_t prev_pid = static_cast<uint32_t>(ss.prev_pid());
  uint32_t next_pid = static_cast<uint32_t>(ss.next_pid());
  SchedEventTracker::GetOrCreate(context_)->PushSchedSwitch(
      cpu, ts, prev_pid, ss.prev_comm(), ss.prev_prio(), ss.prev_state(),
      next_pid, ss.next_comm(), ss.next_prio());
}

void FtraceParser::ParseSchedWakeup(int64_t ts, ConstBytes blob) {
  protos::pbzero::SchedWakeupFtraceEvent::Decoder sw(blob.data, blob.size);
  uint32_t wakee_pid = static_cast<uint32_t>(sw.pid());
  StringId name_id = context_->storage->InternString(sw.comm());
  auto utid = context_->process_tracker->UpdateThreadName(
      wakee_pid, name_id, ThreadNamePriority::kFtrace);
  context_->event_tracker->PushInstant(ts, sched_wakeup_name_id_, utid,
                                       RefType::kRefUtid);
}

void FtraceParser::ParseSchedWaking(int64_t ts, ConstBytes blob) {
  protos::pbzero::SchedWakingFtraceEvent::Decoder sw(blob.data, blob.size);
  uint32_t wakee_pid = static_cast<uint32_t>(sw.pid());
  StringId name_id = context_->storage->InternString(sw.comm());
  auto utid = context_->process_tracker->UpdateThreadName(
      wakee_pid, name_id, ThreadNamePriority::kFtrace);
  context_->event_tracker->PushInstant(ts, sched_waking_name_id_, utid,
                                       RefType::kRefUtid);
}

void FtraceParser::ParseSchedProcessFree(int64_t ts, ConstBytes blob) {
  protos::pbzero::SchedProcessFreeFtraceEvent::Decoder ex(blob.data, blob.size);
  uint32_t pid = static_cast<uint32_t>(ex.pid());
  context_->process_tracker->EndThread(ts, pid);
}

void FtraceParser::ParseCpuFreq(int64_t ts, ConstBytes blob) {
  protos::pbzero::CpuFrequencyFtraceEvent::Decoder freq(blob.data, blob.size);
  uint32_t cpu = freq.cpu_id();
  uint32_t new_freq = freq.state();
  TrackId track =
      context_->track_tracker->InternCpuCounterTrack(cpu_freq_name_id_, cpu);
  context_->event_tracker->PushCounter(ts, new_freq, track);
}

void FtraceParser::ParseGpuFreq(int64_t ts, ConstBytes blob) {
  protos::pbzero::GpuFrequencyFtraceEvent::Decoder freq(blob.data, blob.size);
  uint32_t gpu = freq.gpu_id();
  uint32_t new_freq = freq.state();
  TrackId track =
      context_->track_tracker->InternGpuCounterTrack(gpu_freq_name_id_, gpu);
  context_->event_tracker->PushCounter(ts, new_freq, track);
}

void FtraceParser::ParseCpuIdle(int64_t ts, ConstBytes blob) {
  protos::pbzero::CpuIdleFtraceEvent::Decoder idle(blob.data, blob.size);
  uint32_t cpu = idle.cpu_id();
  uint32_t new_state = idle.state();
  TrackId track =
      context_->track_tracker->InternCpuCounterTrack(cpu_idle_name_id_, cpu);
  context_->event_tracker->PushCounter(ts, new_state, track);
}

void FtraceParser::ParsePrint(int64_t ts, uint32_t pid, ConstBytes blob) {
  protos::pbzero::PrintFtraceEvent::Decoder evt(blob.data, blob.size);
  SystraceParser::GetOrCreate(context_)->ParsePrintEvent(ts, pid, evt.buf());
}

void FtraceParser::ParseZero(int64_t ts, uint32_t pid, ConstBytes blob) {
  protos::pbzero::ZeroFtraceEvent::Decoder evt(blob.data, blob.size);
  uint32_t tgid = static_cast<uint32_t>(evt.pid());
  SystraceParser::GetOrCreate(context_)->ParseZeroEvent(
      ts, pid, evt.flag(), evt.name(), tgid, evt.value());
}

void FtraceParser::ParseSdeTracingMarkWrite(int64_t ts,
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
      ts, pid, static_cast<char>(evt.trace_type()), evt.trace_begin(),
      evt.trace_name(), tgid, evt.value());
}

void FtraceParser::ParseDpuTracingMarkWrite(int64_t ts,
                                            uint32_t pid,
                                            ConstBytes blob) {
  protos::pbzero::DpuTracingMarkWriteFtraceEvent::Decoder evt(blob.data,
                                                              blob.size);
  if (!evt.type()) {
    context_->storage->IncrementStats(stats::systrace_parse_failure);
    return;
  }

  uint32_t tgid = static_cast<uint32_t>(evt.pid());
  SystraceParser::GetOrCreate(context_)->ParseTracingMarkWrite(
      ts, pid, static_cast<char>(evt.type()), false /*trace_begin*/, evt.name(),
      tgid, evt.value());
}

void FtraceParser::ParseG2dTracingMarkWrite(int64_t ts,
                                            uint32_t pid,
                                            ConstBytes blob) {
  protos::pbzero::G2dTracingMarkWriteFtraceEvent::Decoder evt(blob.data,
                                                              blob.size);
  if (!evt.type()) {
    context_->storage->IncrementStats(stats::systrace_parse_failure);
    return;
  }

  uint32_t tgid = static_cast<uint32_t>(evt.pid());
  SystraceParser::GetOrCreate(context_)->ParseTracingMarkWrite(
      ts, pid, static_cast<char>(evt.type()), false /*trace_begin*/, evt.name(),
      tgid, evt.value());
}

/** Parses ion heap events present in Pixel kernels. */
void FtraceParser::ParseIonHeapGrowOrShrink(int64_t ts,
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
    char counter_name[255];
    base::StringView heap_name = ion.heap_name();
    snprintf(counter_name, sizeof(counter_name), "mem.ion.%.*s",
             int(heap_name.size()), heap_name.data());
    global_name_id = context_->storage->InternString(counter_name);
    snprintf(counter_name, sizeof(counter_name), "mem.ion_change.%.*s",
             int(heap_name.size()), heap_name.data());
    change_name_id = context_->storage->InternString(counter_name);
  }

  // Push the global counter.
  TrackId track =
      context_->track_tracker->InternGlobalCounterTrack(global_name_id);
  context_->event_tracker->PushCounter(ts, static_cast<double>(total_bytes),
                                       track);

  // Push the change counter.
  // TODO(b/121331269): these should really be instant events.
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  track =
      context_->track_tracker->InternThreadCounterTrack(change_name_id, utid);
  context_->event_tracker->PushCounter(ts, static_cast<double>(change_bytes),
                                       track);

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
void FtraceParser::ParseIonStat(int64_t ts,
                                uint32_t pid,
                                protozero::ConstBytes data) {
  protos::pbzero::IonStatFtraceEvent::Decoder ion(data.data, data.size);
  // Push the global counter.
  TrackId track =
      context_->track_tracker->InternGlobalCounterTrack(ion_total_id_);
  context_->event_tracker->PushCounter(
      ts, static_cast<double>(ion.total_allocated()), track);

  // Push the change counter.
  // TODO(b/121331269): these should really be instant events.
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  track =
      context_->track_tracker->InternThreadCounterTrack(ion_change_id_, utid);
  context_->event_tracker->PushCounter(ts, static_cast<double>(ion.len()),
                                       track);
}

// This event has both the pid of the thread that sent the signal and the
// destination of the signal. Currently storing the pid of the destination.
void FtraceParser::ParseSignalGenerate(int64_t ts, ConstBytes blob) {
  protos::pbzero::SignalGenerateFtraceEvent::Decoder sig(blob.data, blob.size);

  UniqueTid utid = context_->process_tracker->GetOrCreateThread(
      static_cast<uint32_t>(sig.pid()));
  InstantId id = context_->event_tracker->PushInstant(ts, signal_generate_id_,
                                                      utid, RefType::kRefUtid);

  context_->args_tracker->AddArgsTo(id).AddArg(signal_name_id_,
                                               Variadic::Integer(sig.sig()));
}

void FtraceParser::ParseSignalDeliver(int64_t ts,
                                      uint32_t pid,
                                      ConstBytes blob) {
  protos::pbzero::SignalDeliverFtraceEvent::Decoder sig(blob.data, blob.size);
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  InstantId id = context_->event_tracker->PushInstant(ts, signal_deliver_id_,
                                                      utid, RefType::kRefUtid);

  context_->args_tracker->AddArgsTo(id).AddArg(signal_name_id_,
                                               Variadic::Integer(sig.sig()));
}

void FtraceParser::ParseLowmemoryKill(int64_t ts, ConstBytes blob) {
  // TODO(taylori): Store the pagecache_size, pagecache_limit and free fields
  // in an args table
  protos::pbzero::LowmemoryKillFtraceEvent::Decoder lmk(blob.data, blob.size);

  // Store the pid of the event that is lmk-ed.
  auto pid = static_cast<uint32_t>(lmk.pid());
  auto opt_utid = context_->process_tracker->GetThreadOrNull(pid);

  // Don't add LMK events for threads we've never seen before. This works around
  // the case where we get an LMK event after a thread has already been killed.
  if (!opt_utid)
    return;

  InstantId id = context_->event_tracker->PushInstant(
      ts, lmk_id_, opt_utid.value(), RefType::kRefUtid, true);

  // Store the comm as an arg.
  auto comm_id = context_->storage->InternString(
      lmk.has_comm() ? lmk.comm() : base::StringView());
  context_->args_tracker->AddArgsTo(id).AddArg(comm_name_id_,
                                               Variadic::String(comm_id));
}

void FtraceParser::ParseOOMScoreAdjUpdate(int64_t ts, ConstBytes blob) {
  protos::pbzero::OomScoreAdjUpdateFtraceEvent::Decoder evt(blob.data,
                                                            blob.size);
  // The int16_t static cast is because older version of the on-device tracer
  // had a bug on negative varint encoding (b/120618641).
  int16_t oom_adj = static_cast<int16_t>(evt.oom_score_adj());
  uint32_t tid = static_cast<uint32_t>(evt.pid());
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(tid);
  context_->event_tracker->PushProcessCounterForThread(ts, oom_adj,
                                                       oom_score_adj_id_, utid);
}

void FtraceParser::ParseOOMKill(int64_t ts, ConstBytes blob) {
  protos::pbzero::MarkVictimFtraceEvent::Decoder evt(blob.data, blob.size);
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(
      static_cast<uint32_t>(evt.pid()));
  context_->event_tracker->PushInstant(ts, oom_kill_id_, utid,
                                       RefType::kRefUtid, true);
}

void FtraceParser::ParseMmEventRecord(int64_t ts,
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
      ts, evt.count(), counter_names.count, utid);
  context_->event_tracker->PushProcessCounterForThread(
      ts, evt.max_lat(), counter_names.max_lat, utid);
  context_->event_tracker->PushProcessCounterForThread(
      ts, evt.avg_lat(), counter_names.avg_lat, utid);
}

void FtraceParser::ParseSysEvent(int64_t ts,
                                 uint32_t pid,
                                 bool is_enter,
                                 ConstBytes blob) {
  protos::pbzero::SysEnterFtraceEvent::Decoder evt(blob.data, blob.size);
  uint32_t syscall_num = static_cast<uint32_t>(evt.id());
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);

  SyscallTracker* syscall_tracker = SyscallTracker::GetOrCreate(context_);
  if (is_enter) {
    syscall_tracker->Enter(ts, utid, syscall_num);
  } else {
    syscall_tracker->Exit(ts, utid, syscall_num);
  }

  // We are reusing the same function for sys_enter and sys_exit.
  // It is fine as the arguments are the same, but we need to be sure that the
  // protobuf field id for both are the same.
  static_assert(
      static_cast<int>(protos::pbzero::SysEnterFtraceEvent::kIdFieldNumber) ==
          static_cast<int>(protos::pbzero::SysExitFtraceEvent::kIdFieldNumber),
      "field mismatch");
}

void FtraceParser::ParseTaskNewTask(int64_t ts,
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
  // kthreadd in which case just make it a new thread associated with kthreadd.
  if ((clone_flags & kCloneThread) == 0 && source_tid != kKthreaddPid) {
    // This is a plain-old fork() or equivalent.
    proc_tracker->StartNewProcess(ts, source_tid, new_tid, new_comm);
    return;
  }

  if (source_tid == kKthreaddPid) {
    context_->process_tracker->SetProcessMetadata(
        kKthreaddPid, base::nullopt, kKthreaddName, base::StringView());
  }

  // This is a pthread_create or similar. Bind the two threads together, so
  // they get resolved to the same process.
  auto source_utid = proc_tracker->GetOrCreateThread(source_tid);
  auto new_utid = proc_tracker->StartNewThread(ts, new_tid);
  proc_tracker->UpdateThreadNameByUtid(new_utid, new_comm,
                                       ThreadNamePriority::kFtrace);
  proc_tracker->AssociateThreads(source_utid, new_utid);
}

void FtraceParser::ParseTaskRename(ConstBytes blob) {
  protos::pbzero::TaskRenameFtraceEvent::Decoder evt(blob.data, blob.size);
  uint32_t tid = static_cast<uint32_t>(evt.pid());
  StringId comm = context_->storage->InternString(evt.newcomm());
  context_->process_tracker->UpdateThreadName(tid, comm,
                                              ThreadNamePriority::kFtrace);
  context_->process_tracker->UpdateProcessNameFromThreadName(tid, comm);
}

void FtraceParser::ParseBinderTransaction(int64_t timestamp,
                                          uint32_t pid,
                                          ConstBytes blob) {
  protos::pbzero::BinderTransactionFtraceEvent::Decoder evt(blob.data,
                                                            blob.size);
  int32_t dest_node = static_cast<int32_t>(evt.target_node());
  int32_t dest_tgid = static_cast<int32_t>(evt.to_proc());
  int32_t dest_tid = static_cast<int32_t>(evt.to_thread());
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
  char counter_name[255];
  snprintf(counter_name, sizeof(counter_name), "%.*s %.*s",
           int(clock_name.size()), clock_name.data(), int(subtitle.size()),
           subtitle.data());
  StringId name = context_->storage->InternString(counter_name);
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

void FtraceParser::ParseWorkqueueExecuteStart(int64_t timestamp,
                                              uint32_t pid,
                                              ConstBytes blob) {
  protos::pbzero::WorkqueueExecuteStartFtraceEvent::Decoder evt(blob.data,
                                                                blob.size);
  char slice_name[255];
  snprintf(slice_name, sizeof(slice_name), "%#" PRIx64, evt.function());
  StringId name_id =
      context_->storage->InternString(base::StringView(slice_name));
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track = context_->track_tracker->InternThreadTrack(utid);
  context_->slice_tracker->Begin(timestamp, track, workqueue_id_, name_id);
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
  char track_name[255];
  snprintf(track_name, sizeof(track_name), "Irq Cpu %d", cpu);
  StringId track_name_id = context_->storage->InternString(track_name);
  char slice_name[255];
  base::StringView irq_name = evt.name();
  snprintf(slice_name, sizeof(slice_name), "IRQ (%.*s)", int(irq_name.size()),
           irq_name.data());
  StringId slice_name_id = context_->storage->InternString(slice_name);
  TrackId track = context_->track_tracker->InternCpuTrack(track_name_id, cpu);
  context_->slice_tracker->Begin(timestamp, track, irq_id_, slice_name_id);
}

void FtraceParser::ParseIrqHandlerExit(uint32_t cpu,
                                       int64_t timestamp,
                                       protozero::ConstBytes blob) {
  protos::pbzero::IrqHandlerExitFtraceEvent::Decoder evt(blob.data, blob.size);
  char track_name[255];
  snprintf(track_name, sizeof(track_name), "Irq Cpu %d", cpu);
  StringId track_name_id = context_->storage->InternString(track_name);
  TrackId track = context_->track_tracker->InternCpuTrack(track_name_id, cpu);
  char status[255];
  snprintf(status, sizeof(status), "%s",
           evt.ret() == 1 ? "handled" : "unhandled");
  StringId status_id = context_->storage->InternString(status);
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
  char track_name[255];
  snprintf(track_name, sizeof(track_name), "SoftIrq Cpu %d", cpu);
  StringId track_name_id = context_->storage->InternString(track_name);
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
  char track_name[255];
  snprintf(track_name, sizeof(track_name), "SoftIrq Cpu %d", cpu);
  StringId track_name_id = context_->storage->InternString(track_name);
  TrackId track = context_->track_tracker->InternCpuTrack(track_name_id, cpu);
  auto vec = evt.vec();
  auto args_inserter = [this, vec](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(vec_arg_id_, Variadic::Integer(vec));
  };
  context_->slice_tracker->End(timestamp, track, irq_id_, {}, args_inserter);
}

void FtraceParser::ParseGpuMemTotal(int64_t ts, protozero::ConstBytes data) {
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
    // Process emitting the packet can be different from the pid in the event.
    UniqueTid utid = context_->process_tracker->UpdateThread(pid, pid);
    UniquePid upid = context_->storage->thread_table().upid()[utid].value_or(0);
    track = context_->track_tracker->InternProcessCounterTrack(
        gpu_mem_total_name_id_, upid, gpu_mem_total_unit_id_,
        gpu_mem_total_proc_desc_id_);
  }
  context_->event_tracker->PushCounter(
      ts, static_cast<double>(gpu_mem_total.size()), track);
}

void FtraceParser::ParseThermalTemperature(int64_t timestamp,
                                           protozero::ConstBytes blob) {
  protos::pbzero::ThermalTemperatureFtraceEvent::Decoder evt(blob.data,
                                                             blob.size);
  char counter_name[255];
  base::StringView thermal_zone = evt.thermal_zone();
  snprintf(counter_name, sizeof(counter_name), "%.*s Temperature",
           int(thermal_zone.size()), thermal_zone.data());
  StringId name = context_->storage->InternString(counter_name);
  TrackId track = context_->track_tracker->InternGlobalCounterTrack(name);
  context_->event_tracker->PushCounter(timestamp, evt.temp(), track);
}

void FtraceParser::ParseCdevUpdate(int64_t timestamp,
                                   protozero::ConstBytes blob) {
  protos::pbzero::CdevUpdateFtraceEvent::Decoder evt(blob.data, blob.size);
  char counter_name[255];
  base::StringView type = evt.type();
  snprintf(counter_name, sizeof(counter_name), "%.*s Cooling Device",
           int(type.size()), type.data());
  StringId name = context_->storage->InternString(counter_name);
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
  InstantId id = context_->event_tracker->PushInstant(
      timestamp, sched_blocked_reason_id_, utid, RefType::kRefUtid, false);

  auto inserter = context_->args_tracker->AddArgsTo(id);
  inserter.AddArg(io_wait_id_, Variadic::Boolean(evt.io_wait()));

  uint32_t caller_iid = static_cast<uint32_t>(evt.caller());
  auto* interned_string = seq_state->LookupInternedMessage<
      protos::pbzero::InternedData::kKernelSymbolsFieldNumber,
      protos::pbzero::InternedString>(caller_iid);

  if (interned_string) {
    protozero::ConstBytes str = interned_string->str();
    StringId str_id = context_->storage->InternString(
        base::StringView(reinterpret_cast<const char*>(str.data), str.size));
    inserter.AddArg(function_id_, Variadic::String(str_id));
  }
}

void FtraceParser::ParseFastRpcDmaStat(int64_t timestamp,
                                       uint32_t pid,
                                       protozero::ConstBytes blob) {
  protos::pbzero::FastrpcDmaStatFtraceEvent::Decoder evt(blob.data, blob.size);

  StringId name;
  if (0 <= evt.cid() && evt.cid() < static_cast<int32_t>(kFastRpcCounterSize)) {
    name = fast_rpc_counter_names_[static_cast<size_t>(evt.cid())];
  } else {
    char str[64];
    sprintf(str, "mem.fastrpc[%" PRId32 "]", evt.cid());
    name = context_->storage->InternString(str);
  }

  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track = context_->track_tracker->InternThreadCounterTrack(name, utid);
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(evt.total_allocated()), track);
}

}  // namespace trace_processor
}  // namespace perfetto
