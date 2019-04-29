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

#include "src/trace_processor/proto_trace_parser.h"

#include <inttypes.h>
#include <string.h>

#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/optional.h"
#include "perfetto/base/string_view.h"
#include "perfetto/base/utils.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/traced/sys_stats_counters.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/clock_tracker.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/ftrace_descriptors.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/syscall_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

#include "perfetto/common/android_log_constants.pbzero.h"
#include "perfetto/common/trace_stats.pbzero.h"
#include "perfetto/trace/android/android_log.pbzero.h"
#include "perfetto/trace/clock_snapshot.pbzero.h"
#include "perfetto/trace/ftrace/ftrace.pbzero.h"
#include "perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "perfetto/trace/ftrace/ftrace_stats.pbzero.h"
#include "perfetto/trace/ftrace/generic.pbzero.h"
#include "perfetto/trace/ftrace/kmem.pbzero.h"
#include "perfetto/trace/ftrace/lowmemorykiller.pbzero.h"
#include "perfetto/trace/ftrace/mm_event.pbzero.h"
#include "perfetto/trace/ftrace/oom.pbzero.h"
#include "perfetto/trace/ftrace/power.pbzero.h"
#include "perfetto/trace/ftrace/raw_syscalls.pbzero.h"
#include "perfetto/trace/ftrace/sched.pbzero.h"
#include "perfetto/trace/ftrace/signal.pbzero.h"
#include "perfetto/trace/ftrace/task.pbzero.h"
#include "perfetto/trace/power/battery_counters.pbzero.h"
#include "perfetto/trace/power/power_rails.pbzero.h"
#include "perfetto/trace/profiling/profile_packet.pbzero.h"
#include "perfetto/trace/ps/process_stats.pbzero.h"
#include "perfetto/trace/ps/process_tree.pbzero.h"
#include "perfetto/trace/sys_stats/sys_stats.pbzero.h"
#include "perfetto/trace/system_info.pbzero.h"
#include "perfetto/trace/trace.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {

using protozero::ProtoDecoder;
using Variadic = TraceStorage::Args::Variadic;

}  // namespace

// We have to handle trace_marker events of a few different types:
// 1. some random text
// 2. B|1636|pokeUserActivity
// 3. E|1636
// 4. C|1636|wq:monitor|0
bool ParseSystraceTracePoint(base::StringView str, SystraceTracePoint* out) {
  // THIS char* IS NOT NULL TERMINATED.
  const char* s = str.data();
  size_t len = str.size();

  if (len < 2)
    return false;

  // If str matches '[BEC]\|[0-9]+[\|\n]' set tgid_length to the length of
  // the number. Otherwise return false.
  if (s[1] != '|' && s[1] != '\n')
    return false;
  if (s[0] != 'B' && s[0] != 'E' && s[0] != 'C')
    return false;
  size_t tgid_length = 0;
  for (size_t i = 2; i < len; i++) {
    if (s[i] == '|' || s[i] == '\n') {
      tgid_length = i - 2;
      break;
    }
    if (s[i] < '0' || s[i] > '9')
      return false;
  }

  if (tgid_length == 0) {
    out->tgid = 0;
  } else {
    std::string tgid_str(s + 2, tgid_length);
    out->tgid = static_cast<uint32_t>(std::stoi(tgid_str.c_str()));
  }

  out->phase = s[0];
  switch (s[0]) {
    case 'B': {
      size_t name_index = 2 + tgid_length + 1;
      out->name = base::StringView(
          s + name_index, len - name_index - (s[len - 1] == '\n' ? 1 : 0));
      return true;
    }
    case 'E': {
      return true;
    }
    case 'C': {
      size_t name_index = 2 + tgid_length + 1;
      size_t name_length = 0;
      for (size_t i = name_index; i < len; i++) {
        if (s[i] == '|' || s[i] == '\n') {
          name_length = i - name_index;
          break;
        }
      }
      out->name = base::StringView(s + name_index, name_length);

      size_t value_index = name_index + name_length + 1;
      size_t value_len = len - value_index;
      char value_str[32];
      if (value_len >= sizeof(value_str)) {
        return false;
      }
      memcpy(value_str, s + value_index, value_len);
      value_str[value_len] = 0;
      out->value = std::stod(value_str);
      return true;
    }
    default:
      return false;
  }
}

ProtoTraceParser::ProtoTraceParser(TraceProcessorContext* context)
    : context_(context),
      utid_name_id_(context->storage->InternString("utid")),
      sched_wakeup_name_id_(context->storage->InternString("sched_wakeup")),
      cpu_freq_name_id_(context->storage->InternString("cpufreq")),
      cpu_idle_name_id_(context->storage->InternString("cpuidle")),
      comm_name_id_(context->storage->InternString("comm")),
      num_forks_name_id_(context->storage->InternString("num_forks")),
      num_irq_total_name_id_(context->storage->InternString("num_irq_total")),
      num_softirq_total_name_id_(
          context->storage->InternString("num_softirq_total")),
      num_irq_name_id_(context->storage->InternString("num_irq")),
      num_softirq_name_id_(context->storage->InternString("num_softirq")),
      cpu_times_user_ns_id_(
          context->storage->InternString("cpu.times.user_ns")),
      cpu_times_user_nice_ns_id_(
          context->storage->InternString("cpu.times.user_nice_ns")),
      cpu_times_system_mode_ns_id_(
          context->storage->InternString("cpu.times.system_mode_ns")),
      cpu_times_idle_ns_id_(
          context->storage->InternString("cpu.times.idle_ns")),
      cpu_times_io_wait_ns_id_(
          context->storage->InternString("cpu.times.io_wait_ns")),
      cpu_times_irq_ns_id_(context->storage->InternString("cpu.times.irq_ns")),
      cpu_times_softirq_ns_id_(
          context->storage->InternString("cpu.times.softirq_ns")),
      signal_deliver_id_(context->storage->InternString("signal_deliver")),
      signal_generate_id_(context->storage->InternString("signal_generate")),
      batt_charge_id_(context->storage->InternString("batt.charge_uah")),
      batt_capacity_id_(context->storage->InternString("batt.capacity_pct")),
      batt_current_id_(context->storage->InternString("batt.current_ua")),
      batt_current_avg_id_(
          context->storage->InternString("batt.current.avg_ua")),
      lmk_id_(context->storage->InternString("mem.lmk")),
      oom_score_adj_id_(context->storage->InternString("oom_score_adj")),
      ion_total_unknown_id_(context->storage->InternString("mem.ion.unknown")),
      ion_change_unknown_id_(
          context->storage->InternString("mem.ion_change.unknown")) {
  for (const auto& name : BuildMeminfoCounterNames()) {
    meminfo_strs_id_.emplace_back(context->storage->InternString(name));
  }
  for (const auto& name : BuildVmstatCounterNames()) {
    vmstat_strs_id_.emplace_back(context->storage->InternString(name));
  }
  rss_members_.emplace_back(context->storage->InternString("mem.rss.file"));
  rss_members_.emplace_back(context->storage->InternString("mem.rss.anon"));
  rss_members_.emplace_back(context->storage->InternString("mem.swap"));
  rss_members_.emplace_back(context->storage->InternString("mem.rss.shmem"));
  rss_members_.emplace_back(
      context->storage->InternString("mem.rss.unknown"));  // Keep this last.

  using ProcessStats = protos::pbzero::ProcessStats;
  proc_stats_process_names_[ProcessStats::Process::kVmSizeKbFieldNumber] =
      context->storage->InternString("mem.virt");
  proc_stats_process_names_[ProcessStats::Process::kVmRssKbFieldNumber] =
      context->storage->InternString("mem.rss");
  proc_stats_process_names_[ProcessStats::Process::kRssAnonKbFieldNumber] =
      context->storage->InternString("mem.rss.anon");
  proc_stats_process_names_[ProcessStats::Process::kRssFileKbFieldNumber] =
      context->storage->InternString("mem.rss.file");
  proc_stats_process_names_[ProcessStats::Process::kRssShmemKbFieldNumber] =
      context->storage->InternString("mem.rss.shmem");
  proc_stats_process_names_[ProcessStats::Process::kVmSwapKbFieldNumber] =
      context->storage->InternString("mem.swap");
  proc_stats_process_names_[ProcessStats::Process::kVmLockedKbFieldNumber] =
      context->storage->InternString("mem.locked");
  proc_stats_process_names_[ProcessStats::Process::kVmHwmKbFieldNumber] =
      context->storage->InternString("mem.rss.watermark");
  proc_stats_process_names_[ProcessStats::Process::kOomScoreAdjFieldNumber] =
      oom_score_adj_id_;

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
}

ProtoTraceParser::~ProtoTraceParser() = default;

void ProtoTraceParser::ParseTracePacket(
    int64_t ts,
    TraceSorter::TimestampedTracePiece ttp) {
  PERFETTO_DCHECK(ttp.json_value == nullptr);
  const TraceBlobView& blob = ttp.blob_view;

  protos::pbzero::TracePacket::Decoder packet(blob.data(), blob.length());

  if (packet.has_process_tree())
    ParseProcessTree(packet.process_tree());

  if (packet.has_process_stats())
    ParseProcessStats(ts, packet.process_stats());

  if (packet.has_sys_stats())
    ParseSysStats(ts, packet.sys_stats());

  if (packet.has_battery())
    ParseBatteryCounters(ts, packet.battery());

  if (packet.has_power_rails())
    ParsePowerRails(packet.power_rails());

  if (packet.has_trace_stats())
    ParseTraceStats(packet.trace_stats());

  if (packet.has_ftrace_stats())
    ParseFtraceStats(packet.ftrace_stats());

  if (packet.has_clock_snapshot())
    ParseClockSnapshot(packet.clock_snapshot());

  if (packet.has_android_log())
    ParseAndroidLogPacket(packet.android_log());

  if (packet.has_profile_packet())
    ParseProfilePacket(packet.profile_packet());

  if (packet.has_system_info())
    ParseSystemInfo(packet.system_info());

  // TODO(lalitm): maybe move this to the flush method in the trace processor
  // once we have it. This may reduce performance in the ArgsTracker though so
  // needs to be handled carefully.
  context_->args_tracker->Flush();
  PERFETTO_DCHECK(!packet.bytes_left());
}

void ProtoTraceParser::ParseSysStats(int64_t ts, ConstBytes blob) {
  protos::pbzero::SysStats::Decoder sys_stats(blob.data, blob.size);

  for (auto it = sys_stats.meminfo(); it; ++it) {
    protos::pbzero::SysStats::MeminfoValue::Decoder mi(it->data(), it->size());
    auto key = static_cast<size_t>(mi.key());
    if (PERFETTO_UNLIKELY(key >= meminfo_strs_id_.size())) {
      PERFETTO_ELOG("MemInfo key %zu is not recognized.", key);
      context_->storage->IncrementStats(stats::meminfo_unknown_keys);
      continue;
    }
    // /proc/meminfo counters are in kB, convert to bytes
    context_->event_tracker->PushCounter(
        ts, mi.value() * 1024L, meminfo_strs_id_[key], 0, RefType::kRefNoRef);
  }

  for (auto it = sys_stats.vmstat(); it; ++it) {
    protos::pbzero::SysStats::VmstatValue::Decoder vm(it->data(), it->size());
    auto key = static_cast<size_t>(vm.key());
    if (PERFETTO_UNLIKELY(key >= vmstat_strs_id_.size())) {
      PERFETTO_ELOG("VmStat key %zu is not recognized.", key);
      context_->storage->IncrementStats(stats::vmstat_unknown_keys);
      continue;
    }
    context_->event_tracker->PushCounter(ts, vm.value(), vmstat_strs_id_[key],
                                         0, RefType::kRefNoRef);
  }

  for (auto it = sys_stats.cpu_stat(); it; ++it) {
    protos::pbzero::SysStats::CpuTimes::Decoder ct(it->data(), it->size());
    if (PERFETTO_UNLIKELY(!ct.has_cpu_id())) {
      PERFETTO_ELOG("CPU field not found in CpuTimes");
      context_->storage->IncrementStats(stats::invalid_cpu_times);
      continue;
    }
    context_->event_tracker->PushCounter(ts, ct.user_ns(),
                                         cpu_times_user_ns_id_, ct.cpu_id(),
                                         RefType::kRefCpuId);
    context_->event_tracker->PushCounter(ts, ct.user_ice_ns(),
                                         cpu_times_user_nice_ns_id_,
                                         ct.cpu_id(), RefType::kRefCpuId);
    context_->event_tracker->PushCounter(ts, ct.system_mode_ns(),
                                         cpu_times_system_mode_ns_id_,
                                         ct.cpu_id(), RefType::kRefCpuId);
    context_->event_tracker->PushCounter(ts, ct.idle_ns(),
                                         cpu_times_idle_ns_id_, ct.cpu_id(),
                                         RefType::kRefCpuId);
    context_->event_tracker->PushCounter(ts, ct.io_wait_ns(),
                                         cpu_times_io_wait_ns_id_, ct.cpu_id(),
                                         RefType::kRefCpuId);
    context_->event_tracker->PushCounter(ts, ct.irq_ns(), cpu_times_irq_ns_id_,
                                         ct.cpu_id(), RefType::kRefCpuId);
    context_->event_tracker->PushCounter(ts, ct.softirq_ns(),
                                         cpu_times_softirq_ns_id_, ct.cpu_id(),
                                         RefType::kRefCpuId);
  }

  for (auto it = sys_stats.num_irq(); it; ++it) {
    protos::pbzero::SysStats::InterruptCount::Decoder ic(it->data(),
                                                         it->size());
    context_->event_tracker->PushCounter(ts, ic.count(), num_irq_name_id_,
                                         ic.irq(), RefType::kRefIrq);
  }

  for (auto it = sys_stats.num_softirq(); it; ++it) {
    protos::pbzero::SysStats::InterruptCount::Decoder ic(it->data(),
                                                         it->size());
    context_->event_tracker->PushCounter(ts, ic.count(), num_softirq_name_id_,
                                         ic.irq(), RefType::kRefSoftIrq);
  }

  if (sys_stats.has_num_forks()) {
    context_->event_tracker->PushCounter(
        ts, sys_stats.num_forks(), num_forks_name_id_, 0, RefType::kRefNoRef);
  }

  if (sys_stats.has_num_irq_total()) {
    context_->event_tracker->PushCounter(ts, sys_stats.num_irq_total(),
                                         num_irq_total_name_id_, 0,
                                         RefType::kRefNoRef);
  }

  if (sys_stats.has_num_softirq_total()) {
    context_->event_tracker->PushCounter(ts, sys_stats.num_softirq_total(),
                                         num_softirq_total_name_id_, 0,
                                         RefType::kRefNoRef);
  }
}

void ProtoTraceParser::ParseProcessTree(ConstBytes blob) {
  protos::pbzero::ProcessTree::Decoder ps(blob.data, blob.size);

  for (auto it = ps.processes(); it; ++it) {
    protos::pbzero::ProcessTree::Process::Decoder proc(it->data(), it->size());
    if (!proc.has_cmdline())
      continue;
    auto pid = static_cast<uint32_t>(proc.pid());
    auto ppid = static_cast<uint32_t>(proc.ppid());

    context_->process_tracker->UpdateProcess(pid, ppid,
                                             proc.cmdline()->as_string());
  }

  for (auto it = ps.threads(); it; ++it) {
    protos::pbzero::ProcessTree::Thread::Decoder thd(it->data(), it->size());
    auto tid = static_cast<uint32_t>(thd.tid());
    auto tgid = static_cast<uint32_t>(thd.tgid());
    context_->process_tracker->UpdateThread(tid, tgid);
  }
}

void ProtoTraceParser::ParseProcessStats(int64_t ts, ConstBytes blob) {
  protos::pbzero::ProcessStats::Decoder stats(blob.data, blob.size);
  const auto kOomScoreAdjFieldNumber =
      protos::pbzero::ProcessStats::Process::kOomScoreAdjFieldNumber;
  for (auto it = stats.processes(); it; ++it) {
    // Maps a process counter field it to its value.
    // E.g., 4 := 1024 -> "mem.rss.anon" := 1024.
    std::array<int64_t, kProcStatsProcessSize> counter_values{};
    std::array<bool, kProcStatsProcessSize> has_counter{};

    ProtoDecoder proc(it->data(), it->size());
    uint32_t pid = 0;
    for (auto fld = proc.ReadField(); fld.valid(); fld = proc.ReadField()) {
      if (fld.id() == protos::pbzero::ProcessStats::Process::kPidFieldNumber) {
        pid = fld.as_uint32();
        continue;
      }
      bool is_counter_field = fld.id() < proc_stats_process_names_.size() &&
                              proc_stats_process_names_[fld.id()] != 0;
      if (is_counter_field) {
        // Memory counters are in KB, keep values in bytes in the trace
        // processor.
        counter_values[fld.id()] = fld.id() == kOomScoreAdjFieldNumber
                                       ? fld.as_int64()
                                       : fld.as_int64() * 1024;
        has_counter[fld.id()] = true;
      } else {
        context_->storage->IncrementStats(stats::proc_stat_unknown_counters);
      }
    }

    // Skip field_id 0 (invalid) and 1 (pid).
    for (size_t field_id = 2; field_id < counter_values.size(); field_id++) {
      if (!has_counter[field_id])
        continue;

      // Lookup the interned string id from the field name using the
      // pre-cached |proc_stats_process_names_| map.
      StringId name = proc_stats_process_names_[field_id];
      int64_t value = counter_values[field_id];
      UniquePid upid = context_->process_tracker->GetOrCreateProcess(pid);
      context_->event_tracker->PushCounter(ts, value, name, upid,
                                           RefType::kRefUpid);
    }
  }
}

void ProtoTraceParser::ParseFtracePacket(
    uint32_t cpu,
    int64_t ts,
    TraceSorter::TimestampedTracePiece ttp) {
  PERFETTO_DCHECK(ttp.json_value == nullptr);
  const TraceBlobView& ftrace = ttp.blob_view;

  ProtoDecoder decoder(ftrace.data(), ftrace.length());
  uint64_t raw_pid = 0;
  if (auto pid_field =
          decoder.FindField(protos::pbzero::FtraceEvent::kPidFieldNumber)) {
    raw_pid = pid_field.as_uint64();
  } else {
    PERFETTO_ELOG("Pid field not found in ftrace packet");
    return;
  }
  uint32_t pid = static_cast<uint32_t>(raw_pid);

  for (auto fld = decoder.ReadField(); fld.valid(); fld = decoder.ReadField()) {
    bool is_metadata_field =
        fld.id() == protos::pbzero::FtraceEvent::kPidFieldNumber ||
        fld.id() == protos::pbzero::FtraceEvent::kTimestampFieldNumber;
    if (is_metadata_field)
      continue;

    ConstBytes data = fld.as_bytes();
    if (fld.id() == protos::pbzero::FtraceEvent::kGenericFieldNumber) {
      ParseGenericFtrace(ts, cpu, pid, data);
    } else if (fld.id() !=
               protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber) {
      ParseTypedFtraceToRaw(fld.id(), ts, cpu, pid, data);
    }

    switch (fld.id()) {
      case protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber: {
        ParseSchedSwitch(cpu, ts, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kSchedWakeupFieldNumber: {
        ParseSchedWakeup(ts, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kCpuFrequencyFieldNumber: {
        ParseCpuFreq(ts, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kCpuIdleFieldNumber: {
        ParseCpuIdle(ts, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kPrintFieldNumber: {
        ParsePrint(cpu, ts, pid, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kRssStatFieldNumber: {
        ParseRssStat(ts, pid, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kIonHeapGrowFieldNumber: {
        ParseIonHeapGrowOrShrink(ts, pid, data, true);
        break;
      }
      case protos::pbzero::FtraceEvent::kIonHeapShrinkFieldNumber: {
        ParseIonHeapGrowOrShrink(ts, pid, data, false);
        break;
      }
      case protos::pbzero::FtraceEvent::kSignalGenerateFieldNumber: {
        ParseSignalGenerate(ts, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kSignalDeliverFieldNumber: {
        ParseSignalDeliver(ts, pid, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kLowmemoryKillFieldNumber: {
        ParseLowmemoryKill(ts, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kOomScoreAdjUpdateFieldNumber: {
        ParseOOMScoreAdjUpdate(ts, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kMmEventRecordFieldNumber: {
        ParseMmEventRecord(ts, pid, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kSysEnterFieldNumber: {
        ParseSysEvent(ts, pid, true, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kSysExitFieldNumber: {
        ParseSysEvent(ts, pid, false, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kTaskNewtaskFieldNumber: {
        ParseTaskNewTask(ts, pid, data);
        break;
      }
      case protos::pbzero::FtraceEvent::kTaskRenameFieldNumber: {
        ParseTaskRename(data);
        break;
      }
      default:
        break;
    }
  }
  // TODO(lalitm): maybe move this to the flush method in the trace processor
  // once we have it. This may reduce performance in the ArgsTracker though so
  // needs to be handled carefully.
  context_->args_tracker->Flush();

  PERFETTO_DCHECK(!decoder.bytes_left());
}

void ProtoTraceParser::ParseSignalDeliver(int64_t ts,
                                          uint32_t pid,
                                          ConstBytes blob) {
  protos::pbzero::SignalDeliverFtraceEvent::Decoder sig(blob.data, blob.size);
  auto* instants = context_->storage->mutable_instants();
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  instants->AddInstantEvent(ts, signal_deliver_id_, sig.sig(), utid,
                            RefType::kRefUtid);
}

// This event has both the pid of the thread that sent the signal and the
// destination of the signal. Currently storing the pid of the destination.
void ProtoTraceParser::ParseSignalGenerate(int64_t ts, ConstBytes blob) {
  protos::pbzero::SignalGenerateFtraceEvent::Decoder sig(blob.data, blob.size);

  auto* instants = context_->storage->mutable_instants();
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(
      static_cast<uint32_t>(sig.pid()));
  instants->AddInstantEvent(ts, signal_generate_id_, sig.sig(), utid,
                            RefType::kRefUtid);
}

void ProtoTraceParser::ParseLowmemoryKill(int64_t ts, ConstBytes blob) {
  // TODO(taylori): Store the pagecache_size, pagecache_limit and free fields
  // in an args table
  protos::pbzero::LowmemoryKillFtraceEvent::Decoder lmk(blob.data, blob.size);

  // Store the pid of the event that is lmk-ed.
  auto* instants = context_->storage->mutable_instants();
  auto tid = static_cast<uint32_t>(lmk.pid());
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(tid);
  uint32_t row = instants->AddInstantEvent(ts, lmk_id_, 0, utid,
                                           RefType::kRefUtidLookupUpid);

  // Store the comm as an arg.
  RowId row_id = TraceStorage::CreateRowId(TableId::kInstants, row);
  auto comm_id = context_->storage->InternString(
      lmk.has_comm() ? lmk.comm() : base::StringView());
  context_->args_tracker->AddArg(row_id, comm_name_id_, comm_name_id_,
                                 Variadic::String(comm_id));
}

void ProtoTraceParser::ParseRssStat(int64_t ts, uint32_t pid, ConstBytes blob) {
  protos::pbzero::RssStatFtraceEvent::Decoder rss(blob.data, blob.size);
  const auto kRssStatUnknown = static_cast<uint32_t>(rss_members_.size()) - 1;
  auto member = static_cast<uint32_t>(rss.member());
  int64_t size = rss.size();
  if (member >= rss_members_.size()) {
    context_->storage->IncrementStats(stats::rss_stat_unknown_keys);
    member = kRssStatUnknown;
  }

  if (size >= 0) {
    UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
    context_->event_tracker->PushCounter(ts, size, rss_members_[member], utid,
                                         RefType::kRefUtidLookupUpid);
  } else {
    context_->storage->IncrementStats(stats::rss_stat_negative_size);
  }
}

void ProtoTraceParser::ParseIonHeapGrowOrShrink(int64_t ts,
                                                uint32_t pid,
                                                ConstBytes blob,
                                                bool grow) {
  protos::pbzero::IonHeapGrowFtraceEvent::Decoder ion(blob.data, blob.size);
  int64_t total_bytes = ion.total_allocated();
  int64_t change_bytes = static_cast<int64_t>(ion.len()) * (grow ? 1 : -1);
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
  context_->event_tracker->PushCounter(ts, total_bytes, global_name_id, 0,
                                       RefType::kRefNoRef);

  // Push the change counter.
  // TODO(b/121331269): these should really be instant events. For now we
  // manually reset them to 0 after 1ns.
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  context_->event_tracker->PushCounter(ts, change_bytes, change_name_id, utid,
                                       RefType::kRefUtid);
  context_->event_tracker->PushCounter(ts + 1, 0, change_name_id, utid,
                                       RefType::kRefUtid);

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

void ProtoTraceParser::ParseCpuFreq(int64_t ts, ConstBytes blob) {
  protos::pbzero::CpuFrequencyFtraceEvent::Decoder freq(blob.data, blob.size);
  uint32_t cpu = freq.cpu_id();
  uint32_t new_freq = freq.state();
  context_->event_tracker->PushCounter(ts, new_freq, cpu_freq_name_id_, cpu,
                                       RefType::kRefCpuId);
}

void ProtoTraceParser::ParseCpuIdle(int64_t ts, ConstBytes blob) {
  protos::pbzero::CpuIdleFtraceEvent::Decoder idle(blob.data, blob.size);
  uint32_t cpu = idle.cpu_id();
  uint32_t new_state = idle.state();
  context_->event_tracker->PushCounter(ts, new_state, cpu_idle_name_id_, cpu,
                                       RefType::kRefCpuId);
}

PERFETTO_ALWAYS_INLINE
void ProtoTraceParser::ParseSchedSwitch(uint32_t cpu,
                                        int64_t ts,
                                        ConstBytes blob) {
  protos::pbzero::SchedSwitchFtraceEvent::Decoder ss(blob.data, blob.size);
  uint32_t prev_pid = static_cast<uint32_t>(ss.prev_pid());
  uint32_t next_pid = static_cast<uint32_t>(ss.next_pid());
  context_->event_tracker->PushSchedSwitch(
      cpu, ts, prev_pid, ss.prev_comm(), ss.prev_prio(), ss.prev_state(),
      next_pid, ss.next_comm(), ss.next_prio());
}

void ProtoTraceParser::ParseSchedWakeup(int64_t ts, ConstBytes blob) {
  protos::pbzero::SchedWakeupFtraceEvent::Decoder sw(blob.data, blob.size);
  uint32_t wakee_pid = static_cast<uint32_t>(sw.pid());
  StringId name_id = context_->storage->InternString(sw.comm());
  auto utid = context_->process_tracker->UpdateThreadName(wakee_pid, name_id);
  context_->storage->mutable_instants()->AddInstantEvent(
      ts, sched_wakeup_name_id_, 0 /* value */, utid, RefType::kRefUtid);
}

void ProtoTraceParser::ParseTaskNewTask(int64_t ts,
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
  if ((clone_flags & kCloneThread) == 0) {
    // This is a plain-old fork() or equivalent.
    proc_tracker->StartNewProcess(ts, new_tid);
    return;
  }

  // This is a pthread_create or similar. Bind the two threads together, so
  // they get resolved to the same process.
  auto source_utid = proc_tracker->GetOrCreateThread(source_tid);
  auto new_utid = proc_tracker->StartNewThread(ts, new_tid, new_comm);
  proc_tracker->AssociateThreads(source_utid, new_utid);
}

void ProtoTraceParser::ParseTaskRename(ConstBytes blob) {
  protos::pbzero::TaskRenameFtraceEvent::Decoder evt(blob.data, blob.size);
  uint32_t tid = static_cast<uint32_t>(evt.pid());
  StringId comm = context_->storage->InternString(evt.newcomm());
  context_->process_tracker->UpdateThreadName(tid, comm);
}

void ProtoTraceParser::ParsePrint(uint32_t,
                                  int64_t ts,
                                  uint32_t pid,
                                  ConstBytes blob) {
  protos::pbzero::PrintFtraceEvent::Decoder evt(blob.data, blob.size);
  SystraceTracePoint point{};
  if (!ParseSystraceTracePoint(evt.buf(), &point)) {
    context_->storage->IncrementStats(stats::systrace_parse_failure);
    return;
  }

  switch (point.phase) {
    case 'B': {
      StringId name_id = context_->storage->InternString(point.name);
      context_->slice_tracker->BeginAndroid(ts, pid, point.tgid, 0 /*cat_id*/,
                                            name_id);
      break;
    }

    case 'E': {
      context_->slice_tracker->EndAndroid(ts, pid, point.tgid);
      break;
    }

    case 'C': {
      // LMK events from userspace are hacked as counter events with the "value"
      // of the counter representing the pid of the killed process which is
      // reset to 0 once the kill is complete.
      // Homogenise this with kernel LMK events as an instant event, ignoring
      // the resets to 0.
      if (point.name == "kill_one_process") {
        auto killed_pid = static_cast<uint32_t>(point.value);
        if (killed_pid != 0) {
          UniquePid killed_upid =
              context_->process_tracker->GetOrCreateProcess(killed_pid);
          context_->storage->mutable_instants()->AddInstantEvent(
              ts, lmk_id_, 0, killed_upid, RefType::kRefUpid);
        }
        // TODO(lalitm): we should not add LMK events to the counters table
        // once the UI has support for displaying instants.
      }
      // This is per upid on purpose. Some counters are pushed from arbitrary
      // threads but are really per process.
      UniquePid upid =
          context_->process_tracker->GetOrCreateProcess(point.tgid);
      StringId name_id = context_->storage->InternString(point.name);
      context_->event_tracker->PushCounter(ts, point.value, name_id, upid,
                                           RefType::kRefUpid);
    }
  }
}

void ProtoTraceParser::ParseBatteryCounters(int64_t ts, ConstBytes blob) {
  protos::pbzero::BatteryCounters::Decoder evt(blob.data, blob.size);
  if (evt.has_charge_counter_uah()) {
    context_->event_tracker->PushCounter(
        ts, evt.charge_counter_uah(), batt_charge_id_, 0, RefType::kRefNoRef);
  }
  if (evt.has_capacity_percent()) {
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(evt.capacity_percent()), batt_capacity_id_, 0,
        RefType::kRefNoRef);
  }
  if (evt.has_current_ua()) {
    context_->event_tracker->PushCounter(ts, evt.current_ua(), batt_current_id_,
                                         0, RefType::kRefNoRef);
  }
  if (evt.has_current_avg_ua()) {
    context_->event_tracker->PushCounter(
        ts, evt.current_avg_ua(), batt_current_avg_id_, 0, RefType::kRefNoRef);
  }
}

void ProtoTraceParser::ParsePowerRails(ConstBytes blob) {
  protos::pbzero::PowerRails::Decoder evt(blob.data, blob.size);
  if (evt.has_rail_descriptor()) {
    for (auto it = evt.rail_descriptor(); it; ++it) {
      protos::pbzero::PowerRails::RailDescriptor::Decoder desc(it->data(),
                                                               it->size());
      uint32_t idx = desc.index();
      if (PERFETTO_UNLIKELY(idx > 256)) {
        PERFETTO_DLOG("Skipping excessively large power_rail index %" PRIu32,
                      idx);
        continue;
      }
      if (power_rails_strs_id_.size() <= idx)
        power_rails_strs_id_.resize(idx + 1);
      char counter_name[255];
      snprintf(counter_name, sizeof(counter_name), "power.%.*s_uws",
               int(desc.rail_name().size), desc.rail_name().data);
      power_rails_strs_id_[idx] = context_->storage->InternString(counter_name);
    }
  }

  if (evt.has_energy_data()) {
    for (auto it = evt.energy_data(); it; ++it) {
      protos::pbzero::PowerRails::EnergyData::Decoder desc(it->data(),
                                                           it->size());
      if (desc.index() < power_rails_strs_id_.size()) {
        int64_t ts = static_cast<int64_t>(desc.timestamp_ms()) * 1000000;
        context_->event_tracker->PushCounter(ts, desc.energy(),
                                             power_rails_strs_id_[desc.index()],
                                             0, RefType::kRefNoRef);
      } else {
        context_->storage->IncrementStats(stats::power_rail_unknown_index);
      }
    }
  }
}

void ProtoTraceParser::ParseOOMScoreAdjUpdate(int64_t ts, ConstBytes blob) {
  protos::pbzero::OomScoreAdjUpdateFtraceEvent::Decoder evt(blob.data,
                                                            blob.size);
  // The int16_t static cast is because older version of the on-device tracer
  // had a bug on negative varint encoding (b/120618641).
  int16_t oom_adj = static_cast<int16_t>(evt.oom_score_adj());
  uint32_t pid = static_cast<uint32_t>(evt.pid());
  UniquePid upid = context_->process_tracker->GetOrCreateProcess(pid);
  context_->event_tracker->PushCounter(ts, oom_adj, oom_score_adj_id_, upid,
                                       RefType::kRefUpid);
}

void ProtoTraceParser::ParseMmEventRecord(int64_t ts,
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
  context_->event_tracker->PushCounter(ts, evt.count(), counter_names.count,
                                       utid, RefType::kRefUtidLookupUpid);
  context_->event_tracker->PushCounter(ts, evt.max_lat(), counter_names.max_lat,
                                       utid, RefType::kRefUtidLookupUpid);
  context_->event_tracker->PushCounter(ts, evt.avg_lat(), counter_names.avg_lat,
                                       utid, RefType::kRefUtidLookupUpid);
}

void ProtoTraceParser::ParseSysEvent(int64_t ts,
                                     uint32_t pid,
                                     bool is_enter,
                                     ConstBytes blob) {
  protos::pbzero::SysEnterFtraceEvent::Decoder evt(blob.data, blob.size);
  uint32_t syscall_num = static_cast<uint32_t>(evt.id());
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);

  if (is_enter) {
    context_->syscall_tracker->Enter(ts, utid, syscall_num);
  } else {
    context_->syscall_tracker->Exit(ts, utid, syscall_num);
  }

  // We are reusing the same function for sys_enter and sys_exit.
  // It is fine as the arguments are the same, but we need to be sure that the
  // protobuf field id for both are the same.
  static_assert(
      static_cast<int>(protos::pbzero::SysEnterFtraceEvent::kIdFieldNumber) ==
          static_cast<int>(protos::pbzero::SysExitFtraceEvent::kIdFieldNumber),
      "field mismatch");
}

void ProtoTraceParser::ParseGenericFtrace(int64_t ts,
                                          uint32_t cpu,
                                          uint32_t tid,
                                          ConstBytes blob) {
  protos::pbzero::GenericFtraceEvent::Decoder evt(blob.data, blob.size);
  StringId event_id = context_->storage->InternString(evt.event_name());
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(tid);
  RowId row_id = context_->storage->mutable_raw_events()->AddRawEvent(
      ts, event_id, cpu, utid);

  for (auto it = evt.field(); it; ++it) {
    protos::pbzero::GenericFtraceEvent::Field::Decoder fld(it->data(),
                                                           it->size());
    auto field_name_id = context_->storage->InternString(fld.name());
    if (fld.has_int_value()) {
      context_->args_tracker->AddArg(row_id, field_name_id, field_name_id,
                                     Variadic::Integer(fld.int_value()));
    } else if (fld.has_uint_value()) {
      context_->args_tracker->AddArg(
          row_id, field_name_id, field_name_id,
          Variadic::Integer(static_cast<int64_t>(fld.uint_value())));
    } else if (fld.has_str_value()) {
      StringId str_value = context_->storage->InternString(fld.str_value());
      context_->args_tracker->AddArg(row_id, field_name_id, field_name_id,
                                     Variadic::String(str_value));
    }
  }
}

void ProtoTraceParser::ParseTypedFtraceToRaw(uint32_t ftrace_id,
                                             int64_t ts,
                                             uint32_t cpu,
                                             uint32_t tid,
                                             ConstBytes blob) {
  ProtoDecoder decoder(blob.data, blob.size);
  if (ftrace_id >= GetDescriptorsSize()) {
    PERFETTO_DLOG("Event with id: %d does not exist and cannot be parsed.",
                  ftrace_id);
    return;
  }

  MessageDescriptor* m = GetMessageDescriptorForId(ftrace_id);
  const auto& message_strings = ftrace_message_strings_[ftrace_id];
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(tid);
  RowId raw_event_id = context_->storage->mutable_raw_events()->AddRawEvent(
      ts, message_strings.message_name_id, cpu, utid);
  for (auto fld = decoder.ReadField(); fld.valid(); fld = decoder.ReadField()) {
    if (PERFETTO_UNLIKELY(fld.id() >= kMaxFtraceEventFields)) {
      PERFETTO_DLOG(
          "Skipping ftrace arg - proto field id is too large (%" PRIu16 ")",
          fld.id());
      continue;
    }
    ProtoSchemaType type = m->fields[fld.id()].type;
    StringId name_id = message_strings.field_name_ids[fld.id()];
    switch (type) {
      case ProtoSchemaType::kUint32:
      case ProtoSchemaType::kInt32:
      case ProtoSchemaType::kUint64:
      case ProtoSchemaType::kInt64:
      case ProtoSchemaType::kFixed64:
      case ProtoSchemaType::kFixed32:
      case ProtoSchemaType::kSfixed32:
      case ProtoSchemaType::kSfixed64:
      case ProtoSchemaType::kSint32:
      case ProtoSchemaType::kSint64:
      case ProtoSchemaType::kBool:
      case ProtoSchemaType::kEnum: {
        context_->args_tracker->AddArg(raw_event_id, name_id, name_id,
                                       Variadic::Integer(fld.as_int64()));
        break;
      }
      case ProtoSchemaType::kString:
      case ProtoSchemaType::kBytes: {
        StringId value = context_->storage->InternString(fld.as_string());
        context_->args_tracker->AddArg(raw_event_id, name_id, name_id,
                                       Variadic::String(value));
        break;
      }
      case ProtoSchemaType::kDouble: {
        context_->args_tracker->AddArg(raw_event_id, name_id, name_id,
                                       Variadic::Real(fld.as_double()));
        break;
      }
      case ProtoSchemaType::kFloat: {
        context_->args_tracker->AddArg(
            raw_event_id, name_id, name_id,
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

void ProtoTraceParser::ParseClockSnapshot(ConstBytes blob) {
  protos::pbzero::ClockSnapshot::Decoder evt(blob.data, blob.size);
  int64_t clock_boottime = 0;
  int64_t clock_monotonic = 0;
  int64_t clock_realtime = 0;
  for (auto it = evt.clocks(); it; ++it) {
    protos::pbzero::ClockSnapshot::Clock::Decoder clk(it->data(), it->size());
    if (clk.type() == protos::pbzero::ClockSnapshot::Clock::BOOTTIME) {
      clock_boottime = static_cast<int64_t>(clk.timestamp());
    } else if (clk.type() == protos::pbzero::ClockSnapshot::Clock::REALTIME) {
      clock_realtime = static_cast<int64_t>(clk.timestamp());
    } else if (clk.type() == protos::pbzero::ClockSnapshot::Clock::MONOTONIC) {
      clock_monotonic = static_cast<int64_t>(clk.timestamp());
    }
  }

  // Usually these snapshots come all together.
  PERFETTO_DCHECK(clock_boottime > 0 && clock_monotonic > 0 &&
                  clock_realtime > 0);

  if (clock_boottime <= 0) {
    PERFETTO_ELOG("ClockSnapshot has an invalid BOOTTIME (%" PRId64 ")",
                  clock_boottime);
    context_->storage->IncrementStats(stats::invalid_clock_snapshots);
    return;
  }

  auto* ct = context_->clock_tracker.get();

  // |clock_boottime| is used as the reference trace time.
  ct->SyncClocks(ClockDomain::kBootTime, clock_boottime, clock_boottime);

  if (clock_monotonic > 0)
    ct->SyncClocks(ClockDomain::kMonotonic, clock_monotonic, clock_boottime);

  if (clock_realtime > 0)
    ct->SyncClocks(ClockDomain::kRealTime, clock_realtime, clock_boottime);
}

void ProtoTraceParser::ParseAndroidLogPacket(ConstBytes blob) {
  protos::pbzero::AndroidLogPacket::Decoder packet(blob.data, blob.size);
  for (auto it = packet.events(); it; ++it)
    ParseAndroidLogEvent(it->as_bytes());

  if (packet.has_stats())
    ParseAndroidLogStats(packet.stats());
}

void ProtoTraceParser::ParseAndroidLogEvent(ConstBytes blob) {
  // TODO(primiano): Add events and non-stringified fields to the "raw" table.
  protos::pbzero::AndroidLogPacket::LogEvent::Decoder evt(blob.data, blob.size);
  int64_t ts = static_cast<int64_t>(evt.timestamp());
  uint32_t pid = static_cast<uint32_t>(evt.pid());
  uint32_t tid = static_cast<uint32_t>(evt.tid());
  uint8_t prio = static_cast<uint8_t>(evt.prio());
  StringId tag_id = context_->storage->InternString(
      evt.has_tag() ? evt.tag() : base::StringView());
  StringId msg_id = context_->storage->InternString(
      evt.has_message() ? evt.message() : base::StringView());

  char arg_msg[4096];
  char* arg_str = &arg_msg[0];
  *arg_str = '\0';
  auto arg_avail = [&arg_msg, &arg_str]() {
    return sizeof(arg_msg) - static_cast<size_t>(arg_str - arg_msg);
  };
  for (auto it = evt.args(); it; ++it) {
    protos::pbzero::AndroidLogPacket::LogEvent::Arg::Decoder arg(it->data(),
                                                                 it->size());
    if (!arg.has_name())
      continue;
    arg_str +=
        snprintf(arg_str, arg_avail(),
                 " %.*s=", static_cast<int>(arg.name().size), arg.name().data);
    if (arg.has_string_value()) {
      arg_str += snprintf(arg_str, arg_avail(), "\"%.*s\"",
                          static_cast<int>(arg.string_value().size),
                          arg.string_value().data);
    } else if (arg.has_int_value()) {
      arg_str += snprintf(arg_str, arg_avail(), "%" PRId64, arg.int_value());
    } else if (arg.has_float_value()) {
      arg_str += snprintf(arg_str, arg_avail(), "%f",
                          static_cast<double>(arg.float_value()));
    }
  }

  if (prio == 0)
    prio = protos::pbzero::AndroidLogPriority::PRIO_INFO;

  if (arg_str != &arg_msg[0]) {
    PERFETTO_DCHECK(!msg_id);
    // Skip the first space char (" foo=1 bar=2" -> "foo=1 bar=2").
    msg_id = context_->storage->InternString(&arg_msg[1]);
  }
  UniquePid utid = tid ? context_->process_tracker->UpdateThread(tid, pid) : 0;
  base::Optional<int64_t> opt_trace_time =
      context_->clock_tracker->ToTraceTime(ClockDomain::kRealTime, ts);
  if (!opt_trace_time)
    return;

  // Log events are NOT required to be sorted by trace_time. The virtual table
  // will take care of sorting on-demand.
  context_->storage->mutable_android_log()->AddLogEvent(
      opt_trace_time.value(), utid, prio, tag_id, msg_id);
}

void ProtoTraceParser::ParseAndroidLogStats(ConstBytes blob) {
  protos::pbzero::AndroidLogPacket::Stats::Decoder evt(blob.data, blob.size);
  if (evt.has_num_failed()) {
    context_->storage->SetStats(stats::android_log_num_failed,
                                static_cast<int64_t>(evt.num_failed()));
  }

  if (evt.has_num_skipped()) {
    context_->storage->SetStats(stats::android_log_num_skipped,
                                static_cast<int64_t>(evt.num_skipped()));
  }

  if (evt.has_num_total()) {
    context_->storage->SetStats(stats::android_log_num_total,
                                static_cast<int64_t>(evt.num_total()));
  }
}

void ProtoTraceParser::ParseTraceStats(ConstBytes blob) {
  protos::pbzero::TraceStats::Decoder evt(blob.data, blob.size);
  auto* storage = context_->storage.get();
  storage->SetStats(stats::traced_producers_connected,
                    static_cast<int64_t>(evt.producers_connected()));
  storage->SetStats(stats::traced_data_sources_registered,
                    static_cast<int64_t>(evt.data_sources_registered()));
  storage->SetStats(stats::traced_data_sources_seen,
                    static_cast<int64_t>(evt.data_sources_seen()));
  storage->SetStats(stats::traced_tracing_sessions,
                    static_cast<int64_t>(evt.tracing_sessions()));
  storage->SetStats(stats::traced_total_buffers,
                    static_cast<int64_t>(evt.total_buffers()));
  storage->SetStats(stats::traced_chunks_discarded,
                    static_cast<int64_t>(evt.chunks_discarded()));
  storage->SetStats(stats::traced_patches_discarded,
                    static_cast<int64_t>(evt.patches_discarded()));

  int buf_num = 0;
  for (auto it = evt.buffer_stats(); it; ++it, ++buf_num) {
    protos::pbzero::TraceStats::BufferStats::Decoder buf(it->data(),
                                                         it->size());
    storage->SetIndexedStats(stats::traced_buf_buffer_size, buf_num,
                             static_cast<int64_t>(buf.buffer_size()));
    storage->SetIndexedStats(stats::traced_buf_bytes_written, buf_num,
                             static_cast<int64_t>(buf.bytes_written()));
    storage->SetIndexedStats(stats::traced_buf_bytes_overwritten, buf_num,
                             static_cast<int64_t>(buf.bytes_overwritten()));
    storage->SetIndexedStats(stats::traced_buf_bytes_read, buf_num,
                             static_cast<int64_t>(buf.bytes_read()));
    storage->SetIndexedStats(stats::traced_buf_padding_bytes_written, buf_num,
                             static_cast<int64_t>(buf.padding_bytes_written()));
    storage->SetIndexedStats(stats::traced_buf_padding_bytes_cleared, buf_num,
                             static_cast<int64_t>(buf.padding_bytes_cleared()));
    storage->SetIndexedStats(stats::traced_buf_chunks_written, buf_num,
                             static_cast<int64_t>(buf.chunks_written()));
    storage->SetIndexedStats(stats::traced_buf_chunks_rewritten, buf_num,
                             static_cast<int64_t>(buf.chunks_rewritten()));
    storage->SetIndexedStats(stats::traced_buf_chunks_overwritten, buf_num,
                             static_cast<int64_t>(buf.chunks_overwritten()));
    storage->SetIndexedStats(stats::traced_buf_chunks_discarded, buf_num,
                             static_cast<int64_t>(buf.chunks_discarded()));
    storage->SetIndexedStats(stats::traced_buf_chunks_read, buf_num,
                             static_cast<int64_t>(buf.chunks_read()));
    storage->SetIndexedStats(
        stats::traced_buf_chunks_committed_out_of_order, buf_num,
        static_cast<int64_t>(buf.chunks_committed_out_of_order()));
    storage->SetIndexedStats(stats::traced_buf_write_wrap_count, buf_num,
                             static_cast<int64_t>(buf.write_wrap_count()));
    storage->SetIndexedStats(stats::traced_buf_patches_succeeded, buf_num,
                             static_cast<int64_t>(buf.patches_succeeded()));
    storage->SetIndexedStats(stats::traced_buf_patches_failed, buf_num,
                             static_cast<int64_t>(buf.patches_failed()));
    storage->SetIndexedStats(stats::traced_buf_readaheads_succeeded, buf_num,
                             static_cast<int64_t>(buf.readaheads_succeeded()));
    storage->SetIndexedStats(stats::traced_buf_readaheads_failed, buf_num,
                             static_cast<int64_t>(buf.readaheads_failed()));
  }
}

void ProtoTraceParser::ParseFtraceStats(ConstBytes blob) {
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
    protos::pbzero::FtraceCpuStats::Decoder cpu_stats(it->data(), it->size());
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
    if (oldest_event_ts >= std::numeric_limits<int64_t>::max()) {
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

void ProtoTraceParser::ParseProfilePacket(ConstBytes blob) {
  protos::pbzero::ProfilePacket::Decoder packet(blob.data, blob.size);
  for (auto it = packet.strings(); it; ++it) {
    protos::pbzero::ProfilePacket::InternedString::Decoder entry(it->data(),
                                                                 it->size());

    const char* str = reinterpret_cast<const char*>(entry.str().data);
    context_->storage->InternString(base::StringView(str, entry.str().size));
  }
}

void ProtoTraceParser::ParseSystemInfo(ConstBytes blob) {
  protos::pbzero::SystemInfo::Decoder packet(blob.data, blob.size);
  if (packet.has_utsname()) {
    ConstBytes utsname_blob = packet.utsname();
    protos::pbzero::Utsname::Decoder utsname(utsname_blob.data,
                                             utsname_blob.size);
    base::StringView machine = utsname.machine();
    if (machine == "aarch64" || machine == "armv8l") {
      context_->syscall_tracker->SetArchitecture(kAarch64);
    } else if (machine == "x86_64") {
      context_->syscall_tracker->SetArchitecture(kX86_64);
    } else {
      PERFETTO_ELOG("Unknown architecture %s", machine.ToStdString().c_str());
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
