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
#include "src/trace_processor/trace_processor_context.h"

#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

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
      cpu_times_user_ice_ns_id_(
          context->storage->InternString("cpu.times.user_ice_ns")),
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

  using ProcessStats = protos::ProcessStats;
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

  // TODO(hjd): Add the missing syscalls + fix on over arch.
  sys_name_ids_ = {{context->storage->InternString("sys_restart_syscall"),
                    context->storage->InternString("sys_exit"),
                    context->storage->InternString("sys_fork"),
                    context->storage->InternString("sys_read"),
                    context->storage->InternString("sys_write"),
                    context->storage->InternString("sys_open"),
                    context->storage->InternString("sys_close"),
                    context->storage->InternString("sys_creat"),
                    context->storage->InternString("sys_link"),
                    context->storage->InternString("sys_unlink"),
                    context->storage->InternString("sys_execve"),
                    context->storage->InternString("sys_chdir"),
                    context->storage->InternString("sys_time")}};

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

void ProtoTraceParser::ParseTracePacket(int64_t ts, TraceBlobView packet) {
  ProtoDecoder decoder(packet.data(), packet.length());

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::TracePacket::kProcessTreeFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        ParseProcessTree(packet.slice(fld_off, fld.size()));
        break;
      }
      case protos::TracePacket::kProcessStatsFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        ParseProcessStats(ts, packet.slice(fld_off, fld.size()));
        break;
      }
      case protos::TracePacket::kSysStatsFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        ParseSysStats(ts, packet.slice(fld_off, fld.size()));
        break;
      }
      case protos::TracePacket::kBatteryFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        ParseBatteryCounters(ts, packet.slice(fld_off, fld.size()));
        break;
      }
      case protos::TracePacket::kTraceStatsFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        ParseTraceStats(packet.slice(fld_off, fld.size()));
        break;
      }
      case protos::TracePacket::kFtraceStatsFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        ParseFtraceStats(packet.slice(fld_off, fld.size()));
        break;
      }
      case protos::TracePacket::kClockSnapshotFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        ParseClockSnapshot(packet.slice(fld_off, fld.size()));
        break;
      }
      case protos::TracePacket::kAndroidLogFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        ParseAndroidLogPacket(packet.slice(fld_off, fld.size()));
        break;
      }
      case protos::TracePacket::kProfilePacketFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        ParseProfilePacket(packet.slice(fld_off, fld.size()));
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

  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseSysStats(int64_t ts, TraceBlobView stats) {
  ProtoDecoder decoder(stats.data(), stats.length());
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::SysStats::kMeminfoFieldNumber: {
        const size_t fld_off = stats.offset_of(fld.data());
        ParseMemInfo(ts, stats.slice(fld_off, fld.size()));
        break;
      }
      case protos::SysStats::kVmstatFieldNumber: {
        const size_t fld_off = stats.offset_of(fld.data());
        ParseVmStat(ts, stats.slice(fld_off, fld.size()));
        break;
      }
      case protos::SysStats::kCpuStatFieldNumber: {
        const size_t fld_off = stats.offset_of(fld.data());
        ParseCpuTimes(ts, stats.slice(fld_off, fld.size()));
        break;
      }
      case protos::SysStats::kNumIrqFieldNumber: {
        const size_t fld_off = stats.offset_of(fld.data());
        ParseIrqCount(ts, stats.slice(fld_off, fld.size()),
                      /*is_softirq=*/false);
        break;
      }
      case protos::SysStats::kNumSoftirqFieldNumber: {
        const size_t fld_off = stats.offset_of(fld.data());
        ParseIrqCount(ts, stats.slice(fld_off, fld.size()),
                      /*is_softirq=*/true);
        break;
      }
      case protos::SysStats::kNumForksFieldNumber: {
        context_->event_tracker->PushCounter(
            ts, fld.as_uint32(), num_forks_name_id_, 0, RefType::kRefNoRef);
        break;
      }
      case protos::SysStats::kNumIrqTotalFieldNumber: {
        context_->event_tracker->PushCounter(
            ts, fld.as_uint32(), num_irq_total_name_id_, 0, RefType::kRefNoRef);
        break;
      }
      case protos::SysStats::kNumSoftirqTotalFieldNumber: {
        context_->event_tracker->PushCounter(ts, fld.as_uint32(),
                                             num_softirq_total_name_id_, 0,
                                             RefType::kRefNoRef);
        break;
      }
      default:
        break;
    }
  }
}
void ProtoTraceParser::ParseIrqCount(int64_t ts,
                                     TraceBlobView irq,
                                     bool is_soft) {
  ProtoDecoder decoder(irq.data(), irq.length());
  uint32_t key = 0;
  uint32_t value = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::SysStats::InterruptCount::kIrqFieldNumber:
        key = fld.as_uint32();
        break;
      case protos::SysStats::InterruptCount::kCountFieldNumber:
        value = fld.as_uint32();
        break;
    }
  }
  RefType ref_type = is_soft ? RefType::kRefIrq : RefType::kRefSoftIrq;
  StringId name_id = is_soft ? num_irq_name_id_ : num_softirq_name_id_;
  context_->event_tracker->PushCounter(ts, value, name_id, key, ref_type);
}

void ProtoTraceParser::ParseMemInfo(int64_t ts, TraceBlobView mem) {
  ProtoDecoder decoder(mem.data(), mem.length());
  uint32_t key = 0;
  uint32_t value = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::SysStats::MeminfoValue::kKeyFieldNumber:
        key = fld.as_uint32();
        break;
      case protos::SysStats::MeminfoValue::kValueFieldNumber:
        value = fld.as_uint32();
        break;
    }
  }
  if (PERFETTO_UNLIKELY(key >= meminfo_strs_id_.size())) {
    PERFETTO_ELOG("MemInfo key %d is not recognized.", key);
    context_->storage->IncrementStats(stats::meminfo_unknown_keys);
    return;
  }
  // /proc/meminfo counters are in kB, convert to bytes
  context_->event_tracker->PushCounter(ts, value * 1024L, meminfo_strs_id_[key],
                                       0, RefType::kRefNoRef);
}

void ProtoTraceParser::ParseVmStat(int64_t ts, TraceBlobView stat) {
  ProtoDecoder decoder(stat.data(), stat.length());
  uint32_t key = 0;
  uint32_t value = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::SysStats::VmstatValue::kKeyFieldNumber:
        key = fld.as_uint32();
        break;
      case protos::SysStats::VmstatValue::kValueFieldNumber:
        value = fld.as_uint32();
        break;
    }
  }
  if (PERFETTO_UNLIKELY(key >= vmstat_strs_id_.size())) {
    PERFETTO_ELOG("VmStat key %d is not recognized.", key);
    context_->storage->IncrementStats(stats::vmstat_unknown_keys);
    return;
  }
  context_->event_tracker->PushCounter(ts, value, vmstat_strs_id_[key], 0,
                                       RefType::kRefNoRef);
}

void ProtoTraceParser::ParseCpuTimes(int64_t ts, TraceBlobView cpu_times) {
  ProtoDecoder decoder(cpu_times.data(), cpu_times.length());
  uint64_t raw_cpu = 0;
  uint32_t value = 0;
  // Speculate on CPU being first.
  constexpr auto kCpuFieldTag = protozero::proto_utils::MakeTagVarInt(
      protos::SysStats::CpuTimes::kCpuIdFieldNumber);
  if (cpu_times.length() > 2 && cpu_times.data()[0] == kCpuFieldTag &&
      cpu_times.data()[1] < 0x80) {
    raw_cpu = cpu_times.data()[1];
  } else {
    if (!PERFETTO_LIKELY((
            decoder.FindIntField<protos::SysStats::CpuTimes::kCpuIdFieldNumber>(
                &raw_cpu)))) {
      PERFETTO_ELOG("CPU field not found in CpuTimes");
      context_->storage->IncrementStats(stats::invalid_cpu_times);
      return;
    }
  }

  int64_t cpu = static_cast<int64_t>(raw_cpu);
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::SysStats::CpuTimes::kUserNsFieldNumber: {
        value = fld.as_uint32();
        context_->event_tracker->PushCounter(ts, value, cpu_times_user_ns_id_,
                                             cpu, RefType::kRefCpuId);
        break;
      }
      case protos::SysStats::CpuTimes::kUserIceNsFieldNumber: {
        value = fld.as_uint32();
        context_->event_tracker->PushCounter(
            ts, value, cpu_times_user_ice_ns_id_, cpu, RefType::kRefCpuId);
        break;
      }
      case protos::SysStats::CpuTimes::kSystemModeNsFieldNumber: {
        value = fld.as_uint32();
        context_->event_tracker->PushCounter(
            ts, value, cpu_times_system_mode_ns_id_, cpu, RefType::kRefCpuId);
        break;
      }
      case protos::SysStats::CpuTimes::kIdleNsFieldNumber: {
        value = fld.as_uint32();
        context_->event_tracker->PushCounter(ts, value, cpu_times_idle_ns_id_,
                                             cpu, RefType::kRefCpuId);
        break;
      }
      case protos::SysStats::CpuTimes::kIoWaitNsFieldNumber: {
        value = fld.as_uint32();
        context_->event_tracker->PushCounter(
            ts, value, cpu_times_io_wait_ns_id_, cpu, RefType::kRefCpuId);
        break;
      }
      case protos::SysStats::CpuTimes::kIrqNsFieldNumber: {
        value = fld.as_uint32();
        context_->event_tracker->PushCounter(ts, value, cpu_times_irq_ns_id_,
                                             cpu, RefType::kRefCpuId);
        break;
      }
      case protos::SysStats::CpuTimes::kSoftirqNsFieldNumber: {
        value = fld.as_uint32();
        context_->event_tracker->PushCounter(
            ts, value, cpu_times_softirq_ns_id_, cpu, RefType::kRefCpuId);
        break;
      }
      default:
        break;
    }
  }
}

void ProtoTraceParser::ParseProcessTree(TraceBlobView pstree) {
  ProtoDecoder decoder(pstree.data(), pstree.length());

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    const size_t fld_off = pstree.offset_of(fld.data());
    switch (fld.id) {
      case protos::ProcessTree::kProcessesFieldNumber: {
        ParseProcess(pstree.slice(fld_off, fld.size()));
        break;
      }
      case protos::ProcessTree::kThreadsFieldNumber: {
        ParseThread(pstree.slice(fld_off, fld.size()));
        break;
      }
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseProcessStats(int64_t ts, TraceBlobView stats) {
  ProtoDecoder decoder(stats.data(), stats.length());

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    const size_t fld_off = stats.offset_of(fld.data());
    switch (fld.id) {
      case protos::ProcessStats::kProcessesFieldNumber: {
        ParseProcessStatsProcess(ts, stats.slice(fld_off, fld.size()));
        break;
      }
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseProcessStatsProcess(int64_t ts,
                                                TraceBlobView proc_stat) {
  ProtoDecoder decoder(proc_stat.data(), proc_stat.length());
  uint32_t pid = 0;

  // Maps a process counter field it to its value.
  // E.g., 4 := 1024 -> "mem.rss.anon" := 1024.
  std::array<int64_t, kProcStatsProcessSize> counter_values{};
  std::array<bool, kProcStatsProcessSize> has_counter{};

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::ProcessStats::Process::kPidFieldNumber:
        pid = fld.as_uint32();
        break;
      default: {
        bool is_counter_field = fld.id < has_counter.size() &&
                                proc_stats_process_names_[fld.id] != 0;
        if (is_counter_field) {
          // Memory counters are in KB, keep values in bytes in the trace
          // processor.
          counter_values[fld.id] =
              fld.id == protos::ProcessStats::Process::kOomScoreAdjFieldNumber
                  ? fld.as_int64()
                  : fld.as_int64() * 1024;
          has_counter[fld.id] = true;
        } else {
          context_->storage->IncrementStats(stats::proc_stat_unknown_counters);
        }
        break;
      }
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

    UniquePid upid = context_->process_tracker->UpdateProcess(pid);
    context_->event_tracker->PushCounter(ts, value, name, upid,
                                         RefType::kRefUpid);
  }

  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseThread(TraceBlobView thread) {
  ProtoDecoder decoder(thread.data(), thread.length());
  uint32_t tid = 0;
  uint32_t tgid = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::ProcessTree::Thread::kTidFieldNumber:
        tid = fld.as_uint32();
        break;
      case protos::ProcessTree::Thread::kTgidFieldNumber:
        tgid = fld.as_uint32();
        break;
      default:
        break;
    }
  }
  context_->process_tracker->UpdateThread(tid, tgid);

  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseProcess(TraceBlobView process) {
  ProtoDecoder decoder(process.data(), process.length());

  uint32_t pid = 0;
  uint32_t ppid = 0;
  base::StringView process_name;

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::ProcessTree::Process::kPidFieldNumber:
        pid = fld.as_uint32();
        break;
      case protos::ProcessTree::Process::kPpidFieldNumber:
        ppid = fld.as_uint32();
        break;
      case protos::ProcessTree::Process::kCmdlineFieldNumber:
        if (process_name.empty())  // cmdline is a repeated field.
          process_name = fld.as_string();
        break;
      default:
        break;
    }
  }

  context_->process_tracker->UpdateProcess(pid, ppid, process_name);
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseFtracePacket(uint32_t cpu,
                                         int64_t timestamp,
                                         TraceBlobView ftrace) {
  ProtoDecoder decoder(ftrace.data(), ftrace.length());
  uint64_t raw_pid = 0;
  if (!PERFETTO_LIKELY(
          (decoder.FindIntField<protos::FtraceEvent::kPidFieldNumber>(
              &raw_pid)))) {
    PERFETTO_ELOG("Pid field not found in ftrace packet");
    return;
  }
  uint32_t pid = static_cast<uint32_t>(raw_pid);

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    bool is_metadata_field =
        fld.id == protos::FtraceEvent::kPidFieldNumber ||
        fld.id == protos::FtraceEvent::kTimestampFieldNumber;
    if (is_metadata_field)
      continue;

    const size_t fld_off = ftrace.offset_of(fld.data());
    if (fld.id == protos::FtraceEvent::kGenericFieldNumber) {
      ParseGenericFtrace(timestamp, cpu, pid,
                         ftrace.slice(fld_off, fld.size()));
    } else if (fld.id != protos::FtraceEvent::kSchedSwitchFieldNumber) {
      ParseTypedFtraceToRaw(fld.id, timestamp, cpu, pid,
                            ftrace.slice(fld_off, fld.size()));
    }

    switch (fld.id) {
      case protos::FtraceEvent::kSchedSwitchFieldNumber: {
        ParseSchedSwitch(cpu, timestamp, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kCpuFrequency: {
        ParseCpuFreq(timestamp, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kCpuIdle: {
        ParseCpuIdle(timestamp, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kPrintFieldNumber: {
        ParsePrint(cpu, timestamp, pid, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kRssStatFieldNumber: {
        ParseRssStat(timestamp, pid, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kIonHeapGrow: {
        ParseIonHeapGrowOrShrink(timestamp, pid,
                                 ftrace.slice(fld_off, fld.size()), true);
        break;
      }
      case protos::FtraceEvent::kIonHeapShrink: {
        ParseIonHeapGrowOrShrink(timestamp, pid,
                                 ftrace.slice(fld_off, fld.size()), false);
        break;
      }
      case protos::FtraceEvent::kSignalGenerate: {
        ParseSignalGenerate(timestamp, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kSignalDeliver: {
        ParseSignalDeliver(timestamp, pid, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kLowmemoryKill: {
        ParseLowmemoryKill(timestamp, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kOomScoreAdjUpdate: {
        ParseOOMScoreAdjUpdate(timestamp, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kMmEventRecordFieldNumber: {
        ParseMmEventRecordField(timestamp, pid,
                                ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kSysEnterFieldNumber: {
        ParseSysEvent(timestamp, pid, true, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kSysExitFieldNumber: {
        ParseSysEvent(timestamp, pid, false, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kTaskNewtaskFieldNumber: {
        ParseTaskNewTask(timestamp, pid, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kTaskRenameFieldNumber: {
        ParseTaskRename(timestamp, ftrace.slice(fld_off, fld.size()));
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

  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseSignalDeliver(int64_t timestamp,
                                          uint32_t pid,
                                          TraceBlobView view) {
  ProtoDecoder decoder(view.data(), view.length());
  uint32_t sig = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::SignalDeliverFtraceEvent::kSigFieldNumber:
        sig = fld.as_uint32();
        break;
    }
  }
  auto* instants = context_->storage->mutable_instants();
  UniqueTid utid = context_->process_tracker->UpdateThread(timestamp, pid, 0);
  instants->AddInstantEvent(timestamp, signal_deliver_id_, sig, utid,
                            RefType::kRefUtid);
}

// This event has both the pid of the thread that sent the signal and the
// destination of the signal. Currently storing the pid of the destination.
void ProtoTraceParser::ParseSignalGenerate(int64_t timestamp,
                                           TraceBlobView view) {
  ProtoDecoder decoder(view.data(), view.length());
  uint32_t pid = 0;
  uint32_t sig = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::SignalGenerateFtraceEvent::kPidFieldNumber:
        pid = fld.as_uint32();
        break;
      case protos::SignalGenerateFtraceEvent::kSigFieldNumber:
        sig = fld.as_uint32();
        break;
    }
  }
  auto* instants = context_->storage->mutable_instants();
  UniqueTid utid = context_->process_tracker->UpdateThread(timestamp, pid, 0);
  instants->AddInstantEvent(timestamp, signal_generate_id_, sig, utid,
                            RefType::kRefUtid);
}

void ProtoTraceParser::ParseLowmemoryKill(int64_t timestamp,
                                          TraceBlobView view) {
  // TODO(taylori): Store the pagecache_size, pagecache_limit and free fields
  // in an args table
  ProtoDecoder decoder(view.data(), view.length());
  uint32_t pid = 0;
  base::StringView comm;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::LowmemoryKillFtraceEvent::kPidFieldNumber:
        pid = fld.as_uint32();
        break;
      case protos::LowmemoryKillFtraceEvent::kCommFieldNumber:
        comm = fld.as_string();
        break;
    }
  }

  // Storing the pid of the event that is lmk-ed.
  auto* instants = context_->storage->mutable_instants();
  UniquePid upid = context_->process_tracker->UpdateProcess(pid);
  uint32_t row = instants->AddInstantEvent(timestamp, lmk_id_, 0, upid,
                                           RefType::kRefUtidLookupUpid);

  // Store the comm as an arg.
  RowId row_id = TraceStorage::CreateRowId(TableId::kInstants, row);
  auto comm_id = context_->storage->InternString(comm);
  context_->args_tracker->AddArg(row_id, comm_name_id_, comm_name_id_,
                                 Variadic::String(comm_id));
}

void ProtoTraceParser::ParseRssStat(int64_t timestamp,
                                    uint32_t pid,
                                    TraceBlobView view) {
  ProtoDecoder decoder(view.data(), view.length());
  const auto kRssStatUnknown = static_cast<uint32_t>(rss_members_.size()) - 1;
  uint32_t member = kRssStatUnknown;
  int64_t size = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::RssStatFtraceEvent::kMemberFieldNumber:
        member = fld.as_uint32();
        break;
      case protos::RssStatFtraceEvent::kSizeFieldNumber:
        size = fld.as_int64();
        break;
    }
  }
  if (member >= rss_members_.size()) {
    context_->storage->IncrementStats(stats::rss_stat_unknown_keys);
    member = kRssStatUnknown;
  }

  if (size >= 0) {
    UniqueTid utid = context_->process_tracker->UpdateThread(timestamp, pid, 0);

    context_->event_tracker->PushCounter(timestamp, size, rss_members_[member],
                                         utid, RefType::kRefUtidLookupUpid);
  } else {
    context_->storage->IncrementStats(stats::rss_stat_negative_size);
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseIonHeapGrowOrShrink(int64_t timestamp,
                                                uint32_t pid,
                                                TraceBlobView view,
                                                bool grow) {
  ProtoDecoder decoder(view.data(), view.length());
  int64_t total_bytes = 0;
  int64_t change_bytes = 0;
  StringId global_name_id = ion_total_unknown_id_;
  StringId change_name_id = ion_change_unknown_id_;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::IonHeapGrowFtraceEvent::kTotalAllocatedFieldNumber:
        total_bytes = fld.as_int64();
        break;
      case protos::IonHeapGrowFtraceEvent::kLenFieldNumber:
        change_bytes = fld.as_int64() * (grow ? 1 : -1);
        break;
      case protos::IonHeapGrowFtraceEvent::kHeapNameFieldNumber: {
        char counter_name[255];
        base::StringView heap_name = fld.as_string();
        snprintf(counter_name, sizeof(counter_name), "mem.ion.%.*s",
                 int(heap_name.size()), heap_name.data());
        global_name_id = context_->storage->InternString(counter_name);

        snprintf(counter_name, sizeof(counter_name), "mem.ion_change.%.*s",
                 int(heap_name.size()), heap_name.data());
        change_name_id = context_->storage->InternString(counter_name);
        break;
      }
    }
  }
  // Push the global counter.
  context_->event_tracker->PushCounter(timestamp, total_bytes, global_name_id,
                                       0, RefType::kRefNoRef);

  // Push the change counter.
  // TODO(b/121331269): these should really be instant events. For now we
  // manually reset them to 0 after 1ns.
  UniqueTid utid = context_->process_tracker->UpdateThread(timestamp, pid, 0);
  context_->event_tracker->PushCounter(timestamp, change_bytes, change_name_id,
                                       utid, RefType::kRefUtid);
  context_->event_tracker->PushCounter(timestamp + 1, 0, change_name_id, utid,
                                       RefType::kRefUtid);
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());

  // We are reusing the same function for ion_heap_grow and ion_heap_shrink.
  // It is fine as the arguments are the same, but we need to be sure that the
  // protobuf field id for both are the same.
  static_assert(
      protos::IonHeapGrowFtraceEvent::kTotalAllocatedFieldNumber ==
              protos::IonHeapShrinkFtraceEvent::kTotalAllocatedFieldNumber &&
          protos::IonHeapGrowFtraceEvent::kLenFieldNumber ==
              protos::IonHeapShrinkFtraceEvent::kLenFieldNumber &&
          protos::IonHeapGrowFtraceEvent::kHeapNameFieldNumber ==
              protos::IonHeapShrinkFtraceEvent::kHeapNameFieldNumber,
      "field mismatch");
}

void ProtoTraceParser::ParseCpuFreq(int64_t timestamp, TraceBlobView view) {
  ProtoDecoder decoder(view.data(), view.length());

  uint32_t cpu_affected = 0;
  uint32_t new_freq = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::CpuFrequencyFtraceEvent::kCpuIdFieldNumber:
        cpu_affected = fld.as_uint32();
        break;
      case protos::CpuFrequencyFtraceEvent::kStateFieldNumber:
        new_freq = fld.as_uint32();
        break;
    }
  }
  context_->event_tracker->PushCounter(timestamp, new_freq, cpu_freq_name_id_,
                                       cpu_affected, RefType::kRefCpuId);
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseCpuIdle(int64_t timestamp, TraceBlobView view) {
  ProtoDecoder decoder(view.data(), view.length());

  uint32_t cpu_affected = 0;
  uint32_t new_state = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::CpuIdleFtraceEvent::kCpuIdFieldNumber:
        cpu_affected = fld.as_uint32();
        break;
      case protos::CpuIdleFtraceEvent::kStateFieldNumber:
        new_state = fld.as_uint32();
        break;
    }
  }
  context_->event_tracker->PushCounter(timestamp, new_state, cpu_idle_name_id_,
                                       cpu_affected, RefType::kRefCpuId);
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseSchedSwitch(uint32_t cpu,
                                        int64_t timestamp,
                                        TraceBlobView sswitch) {
  ProtoDecoder decoder(sswitch.data(), sswitch.length());

  base::StringView prev_comm;
  uint32_t prev_pid = 0;
  int32_t prev_prio = 0;
  int64_t prev_state = 0;
  base::StringView next_comm;
  uint32_t next_pid = 0;
  int32_t next_prio = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::SchedSwitchFtraceEvent::kPrevPidFieldNumber:
        prev_pid = fld.as_uint32();
        break;
      case protos::SchedSwitchFtraceEvent::kPrevStateFieldNumber:
        prev_state = fld.as_int64();
        break;
      case protos::SchedSwitchFtraceEvent::kPrevCommFieldNumber:
        prev_comm = fld.as_string();
        break;
      case protos::SchedSwitchFtraceEvent::kPrevPrioFieldNumber:
        prev_prio = fld.as_int32();
        break;
      case protos::SchedSwitchFtraceEvent::kNextPidFieldNumber:
        next_pid = fld.as_uint32();
        break;
      case protos::SchedSwitchFtraceEvent::kNextCommFieldNumber:
        next_comm = fld.as_string();
        break;
      case protos::SchedSwitchFtraceEvent::kNextPrioFieldNumber:
        next_prio = fld.as_int32();
        break;
      default:
        break;
    }
  }
  context_->event_tracker->PushSchedSwitch(cpu, timestamp, prev_pid, prev_comm,
                                           prev_prio, prev_state, next_pid,
                                           next_comm, next_prio);
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseTaskNewTask(int64_t timestamp,
                                        uint32_t source_tid,
                                        TraceBlobView event) {
  ProtoDecoder decoder(event.data(), event.length());
  uint32_t clone_flags = 0;
  uint32_t new_tid = 0;
  StringId new_comm = 0;

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::TaskNewtaskFtraceEvent::kCloneFlagsFieldNumber:
        clone_flags = fld.as_uint32();
        break;
      case protos::TaskNewtaskFtraceEvent::kPidFieldNumber:
        new_tid = fld.as_uint32();
        break;
      case protos::TaskNewtaskFtraceEvent::kCommFieldNumber:
        new_comm = context_->storage->InternString(fld.as_string());
        break;
      default:
        break;
    }
  }

  auto* proc_tracker = context_->process_tracker.get();

  // task_newtask is raised both in the case of a new process creation (fork()
  // family) and thread creation (clone(CLONE_THREAD, ...)).
  static const uint32_t kCloneThread = 0x00010000;  // From kernel's sched.h.
  if ((clone_flags & kCloneThread) == 0) {
    // This is a plain-old fork() or equivalent.
    proc_tracker->StartNewProcess(timestamp, new_tid);
    return;
  }

  // This is a pthread_create or similar. Bind the two threads together, so
  // they get resolved to the same process.
  auto source_utid = proc_tracker->UpdateThread(timestamp, source_tid, 0);
  auto new_utid = proc_tracker->StartNewThread(timestamp, new_tid, new_comm);
  proc_tracker->AssociateThreads(source_utid, new_utid);
}

void ProtoTraceParser::ParseTaskRename(int64_t timestamp, TraceBlobView event) {
  ProtoDecoder decoder(event.data(), event.length());
  uint32_t tid = 0;
  StringId comm = 0;

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::TaskRenameFtraceEvent::kPidFieldNumber:
        tid = fld.as_uint32();
        break;
      case protos::TaskRenameFtraceEvent::kNewcommFieldNumber:
        comm = context_->storage->InternString(fld.as_string());
        break;
      default:
        break;
    }
  }

  context_->process_tracker->UpdateThread(timestamp, tid, comm);
}

void ProtoTraceParser::ParsePrint(uint32_t,
                                  int64_t timestamp,
                                  uint32_t pid,
                                  TraceBlobView print) {
  ProtoDecoder decoder(print.data(), print.length());

  base::StringView buf{};
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    if (fld.id == protos::PrintFtraceEvent::kBufFieldNumber) {
      buf = fld.as_string();
      break;
    }
  }

  SystraceTracePoint point{};
  if (!ParseSystraceTracePoint(buf, &point))
    return;

  switch (point.phase) {
    case 'B': {
      StringId name_id = context_->storage->InternString(point.name);
      context_->slice_tracker->BeginAndroid(timestamp, pid, point.tgid,
                                            0 /*cat_id*/, name_id);
      break;
    }

    case 'E': {
      context_->slice_tracker->EndAndroid(timestamp, pid, point.tgid);
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
              context_->process_tracker->UpdateProcess(killed_pid);
          context_->storage->mutable_instants()->AddInstantEvent(
              timestamp, lmk_id_, 0, killed_upid, RefType::kRefUpid);
        }
        // TODO(lalitm): we should not add LMK events to the counters table
        // once the UI has support for displaying instants.
      }
      // This is per upid on purpose. Some counters are pushed from arbitrary
      // threads but are really per process.
      UniquePid upid = context_->process_tracker->UpdateProcess(point.tgid);
      StringId name_id = context_->storage->InternString(point.name);
      context_->event_tracker->PushCounter(timestamp, point.value, name_id,
                                           upid, RefType::kRefUpid);
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseBatteryCounters(int64_t ts, TraceBlobView battery) {
  ProtoDecoder decoder(battery.data(), battery.length());
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::BatteryCounters::kChargeCounterUahFieldNumber:
        context_->event_tracker->PushCounter(
            ts, fld.as_int64(), batt_charge_id_, 0, RefType::kRefNoRef);
        break;
      case protos::BatteryCounters::kCapacityPercentFieldNumber:
        context_->event_tracker->PushCounter(
            ts, static_cast<double>(fld.as_float()), batt_capacity_id_, 0,
            RefType::kRefNoRef);
        break;
      case protos::BatteryCounters::kCurrentUaFieldNumber:
        context_->event_tracker->PushCounter(
            ts, fld.as_int64(), batt_current_id_, 0, RefType::kRefNoRef);
        break;
      case protos::BatteryCounters::kCurrentAvgUaFieldNumber:
        context_->event_tracker->PushCounter(
            ts, fld.as_int64(), batt_current_avg_id_, 0, RefType::kRefNoRef);
        break;
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseOOMScoreAdjUpdate(int64_t ts,
                                              TraceBlobView oom_update) {
  ProtoDecoder decoder(oom_update.data(), oom_update.length());
  uint32_t pid = 0;
  int16_t oom_adj = 0;

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::OomScoreAdjUpdateFtraceEvent::kOomScoreAdjFieldNumber:
        // TODO(b/120618641): The int16_t static cast is required because of
        // the linked negative varint encoding bug.
        oom_adj = static_cast<int16_t>(fld.as_int32());
        break;
      case protos::OomScoreAdjUpdateFtraceEvent::kPidFieldNumber:
        pid = fld.as_uint32();
        break;
      case protos::OomScoreAdjUpdateFtraceEvent::kCommFieldNumber:
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());

  UniquePid upid = context_->process_tracker->UpdateProcess(pid);
  context_->event_tracker->PushCounter(ts, oom_adj, oom_score_adj_id_, upid,
                                       RefType::kRefUpid);
}

void ProtoTraceParser::ParseMmEventRecordField(int64_t ts,
                                               uint32_t pid,
                                               TraceBlobView view) {
  ProtoDecoder decoder(view.data(), view.length());

  uint32_t type = 0;
  uint32_t count = 0;
  uint32_t max_lat = 0;
  uint32_t avg_lat = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::MmEventRecordFtraceEvent::kTypeFieldNumber:
        type = fld.as_uint32();
        break;
      case protos::MmEventRecordFtraceEvent::kCountFieldNumber:
        count = fld.as_uint32();
        break;
      case protos::MmEventRecordFtraceEvent::kMaxLatFieldNumber:
        max_lat = fld.as_uint32();
        break;
      case protos::MmEventRecordFtraceEvent::kAvgLatFieldNumber:
        avg_lat = fld.as_uint32();
        break;
      default:
        context_->storage->IncrementStats(stats::mm_unknown_counter);
        break;
    }
  }

  UniqueTid utid = context_->process_tracker->UpdateThread(ts, pid, 0);
  if (type >= mm_event_counter_names_.size()) {
    context_->storage->IncrementStats(stats::mm_unknown_type);
    return;
  }

  const auto& counter_names = mm_event_counter_names_[type];
  context_->event_tracker->PushCounter(ts, count, counter_names.count, utid,
                                       RefType::kRefUtidLookupUpid);
  context_->event_tracker->PushCounter(ts, max_lat, counter_names.max_lat, utid,
                                       RefType::kRefUtidLookupUpid);
  context_->event_tracker->PushCounter(ts, avg_lat, counter_names.avg_lat, utid,
                                       RefType::kRefUtidLookupUpid);

  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseSysEvent(int64_t ts,
                                     uint32_t pid,
                                     bool is_enter,
                                     TraceBlobView view) {
  ProtoDecoder decoder(view.data(), view.length());

  uint32_t id = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::SysEnterFtraceEvent::kIdFieldNumber:
        id = fld.as_uint32();
        break;
    }
  }

  if (id >= sys_name_ids_.size()) {
    context_->storage->IncrementStats(stats::sys_unknown_sys_id);
    return;
  }

  // We see two write sys calls around each userspace slice that is going via
  // trace_marker, this violates the assumption that userspace slices are
  // perfectly nested. For the moment ignore all write sys calls.
  // TODO(hjd): Remove this limitation.
  if (id == 4 /*sys_write*/)
    return;

  StringId sys_name_id = sys_name_ids_[id];
  UniqueTid utid = context_->process_tracker->UpdateThread(ts, pid, 0);
  if (is_enter) {
    context_->slice_tracker->Begin(ts, utid, 0 /* cat */, sys_name_id);
  } else {
    context_->slice_tracker->End(ts, utid, 0 /* cat */, sys_name_id);
  }

  // We are reusing the same function for sys_enter and sys_exit.
  // It is fine as the arguments are the same, but we need to be sure that the
  // protobuf field id for both are the same.
  static_assert(protos::SysEnterFtraceEvent::kIdFieldNumber ==
                    protos::SysExitFtraceEvent::kIdFieldNumber,
                "field mismatch");
}

void ProtoTraceParser::ParseGenericFtrace(int64_t timestamp,
                                          uint32_t cpu,
                                          uint32_t tid,
                                          TraceBlobView view) {
  ProtoDecoder decoder(view.data(), view.length());

  base::StringView event_name;
  if (!PERFETTO_LIKELY((decoder.FindStringField<
                        protos::GenericFtraceEvent::kEventNameFieldNumber>(
          &event_name)))) {
    PERFETTO_ELOG("Event name not found in generic ftrace packet");
    return;
  }

  UniqueTid utid = context_->process_tracker->UpdateThread(timestamp, tid, 0);
  StringId event_id = context_->storage->InternString(std::move(event_name));
  RowId row_id = context_->storage->mutable_raw_events()->AddRawEvent(
      timestamp, event_id, cpu, utid);

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::GenericFtraceEvent::kFieldFieldNumber:
        const size_t fld_off = view.offset_of(fld.data());
        ParseGenericFtraceField(row_id, view.slice(fld_off, fld.size()));
        break;
    }
  }
}

void ProtoTraceParser::ParseGenericFtraceField(RowId generic_row_id,
                                               TraceBlobView view) {
  ProtoDecoder decoder(view.data(), view.length());

  base::StringView field_name;
  if (!PERFETTO_LIKELY((decoder.FindStringField<
                        protos::GenericFtraceEvent::Field::kNameFieldNumber>(
          &field_name)))) {
    PERFETTO_ELOG("Event name not found in generic ftrace packet");
    return;
  }
  auto field_name_id = context_->storage->InternString(std::move(field_name));
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::GenericFtraceEvent::Field::kIntValue:
      case protos::GenericFtraceEvent::Field::kUintValue: {
        context_->args_tracker->AddArg(generic_row_id, field_name_id,
                                       field_name_id,
                                       Variadic::Integer(fld.as_integer()));
        break;
      }
      case protos::GenericFtraceEvent::Field::kStrValue: {
        StringId value = context_->storage->InternString(fld.as_string());
        context_->args_tracker->AddArg(generic_row_id, field_name_id,
                                       field_name_id, Variadic::String(value));
      }
    }
  }
}

void ProtoTraceParser::ParseTypedFtraceToRaw(uint32_t ftrace_id,
                                             int64_t timestamp,
                                             uint32_t cpu,
                                             uint32_t tid,
                                             TraceBlobView view) {
  ProtoDecoder decoder(view.data(), view.length());
  if (ftrace_id >= GetDescriptorsSize()) {
    PERFETTO_DLOG("Event with id: %d does not exist and cannot be parsed.",
                  ftrace_id);
    return;
  }

  MessageDescriptor* m = GetMessageDescriptorForId(ftrace_id);
  const auto& message_strings = ftrace_message_strings_[ftrace_id];
  UniqueTid utid = context_->process_tracker->UpdateThread(timestamp, tid, 0);
  RowId raw_event_id = context_->storage->mutable_raw_events()->AddRawEvent(
      timestamp, message_strings.message_name_id, cpu, utid);
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    ProtoSchemaType type = m->fields[fld.id].type;
    StringId name_id = message_strings.field_name_ids[fld.id];
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
                                       Variadic::Integer(fld.as_integer()));
        break;
      }
      case ProtoSchemaType::kString:
      case ProtoSchemaType::kBytes: {
        StringId value = context_->storage->InternString(fld.as_string());
        context_->args_tracker->AddArg(raw_event_id, name_id, name_id,
                                       Variadic::String(value));
        break;
      }
      case ProtoSchemaType::kDouble:
      case ProtoSchemaType::kFloat: {
        context_->args_tracker->AddArg(raw_event_id, name_id, name_id,
                                       Variadic::Real(fld.as_real()));
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

void ProtoTraceParser::ParseClockSnapshot(TraceBlobView packet) {
  ProtoDecoder decoder(packet.data(), packet.length());
  int64_t clock_boottime = 0;
  int64_t clock_monotonic = 0;
  int64_t clock_realtime = 0;

  // This loop iterates over the "repeated Clock" entries.
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::ClockSnapshot::kClocksFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        auto clk = ParseClockField(packet.slice(fld_off, fld.size()));
        switch (clk.first) {
          case protos::ClockSnapshot::Clock::BOOTTIME:
            clock_boottime = clk.second;
            break;
          case protos::ClockSnapshot::Clock::REALTIME:
            clock_realtime = clk.second;
            break;
          case protos::ClockSnapshot::Clock::MONOTONIC:
            clock_monotonic = clk.second;
            break;
        }
        break;
      }
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());

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

std::pair<int, int64_t> ProtoTraceParser::ParseClockField(
    TraceBlobView packet) {
  ProtoDecoder decoder(packet.data(), packet.length());
  int type = protos::ClockSnapshot::Clock::UNKNOWN;
  int64_t value = -1;

  // This loop iterates over the |type| and |timestamp| field of each
  // clock snapshot.
  for (auto fld = decoder.ReadField(); fld.id; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::ClockSnapshot::Clock::kTypeFieldNumber:
        type = fld.as_int32();
        break;
      case protos::ClockSnapshot::Clock::kTimestampFieldNumber:
        value = fld.as_int64();
        break;
    }
  }
  return std::make_pair(type, value);
}

void ProtoTraceParser::ParseAndroidLogPacket(TraceBlobView packet) {
  ProtoDecoder decoder(packet.data(), packet.length());
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::AndroidLogPacket::kEventsFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        ParseAndroidLogEvent(packet.slice(fld_off, fld.size()));
        break;
      }
      case protos::AndroidLogPacket::kStatsFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        ParseAndroidLogStats(packet.slice(fld_off, fld.size()));
        break;
      }
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseAndroidLogEvent(TraceBlobView event) {
  // TODO(primiano): Add events and non-stringified fields to the "raw" table.
  ProtoDecoder decoder(event.data(), event.length());
  int64_t ts = 0;
  uint32_t pid = 0;
  uint32_t tid = 0;
  uint8_t prio = 0;
  StringId tag_id = 0;
  StringId msg_id = 0;
  char arg_msg[4096];
  char* arg_str = &arg_msg[0];
  *arg_str = '\0';
  auto arg_avail = [&arg_msg, &arg_str]() {
    return sizeof(arg_msg) - static_cast<size_t>(arg_str - arg_msg);
  };

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::AndroidLogPacket::LogEvent::kPidFieldNumber:
        pid = fld.as_uint32();
        break;
      case protos::AndroidLogPacket::LogEvent::kTidFieldNumber:
        tid = fld.as_uint32();
        break;
      case protos::AndroidLogPacket::LogEvent::kTimestampFieldNumber:
        ts = fld.as_int64();
        break;
      case protos::AndroidLogPacket::LogEvent::kPrioFieldNumber:
        prio = static_cast<uint8_t>(fld.as_uint32());
        break;
      case protos::AndroidLogPacket::LogEvent::kTagFieldNumber:
        tag_id = context_->storage->InternString(fld.as_string());
        break;
      case protos::AndroidLogPacket::LogEvent::kMessageFieldNumber:
        msg_id = context_->storage->InternString(fld.as_string());
        break;
      case protos::AndroidLogPacket::LogEvent::kArgsFieldNumber: {
        const size_t fld_off = event.offset_of(fld.data());
        TraceBlobView arg_data = event.slice(fld_off, fld.size());
        ParseAndroidLogBinaryArg(std::move(arg_data), &arg_str, arg_avail());
        break;
      }
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());

  if (prio == 0)
    prio = protos::AndroidLogPriority::PRIO_INFO;

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

void ProtoTraceParser::ParseAndroidLogBinaryArg(TraceBlobView arg,
                                                char** str,
                                                size_t avail) {
  ProtoDecoder decoder(arg.data(), arg.length());
  for (auto fld = decoder.ReadField(); fld.id; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::AndroidLogPacket::LogEvent::Arg::kNameFieldNumber: {
        base::StringView name = fld.as_string();
        *str += snprintf(*str, avail, " %.*s=", static_cast<int>(name.size()),
                         name.data());
        break;
      }
      case protos::AndroidLogPacket::LogEvent::Arg::kStringValueFieldNumber: {
        base::StringView val = fld.as_string();
        *str += snprintf(*str, avail, "\"%.*s\"", static_cast<int>(val.size()),
                         val.data());
        break;
      }
      case protos::AndroidLogPacket::LogEvent::Arg::kIntValueFieldNumber:
        *str += snprintf(*str, avail, "%" PRId64, fld.as_int64());
        break;
      case protos::AndroidLogPacket::LogEvent::Arg::kFloatValueFieldNumber:
        *str +=
            snprintf(*str, avail, "%f", static_cast<double>(fld.as_float()));
        break;
    }
  }
}

void ProtoTraceParser::ParseAndroidLogStats(TraceBlobView packet) {
  ProtoDecoder decoder(packet.data(), packet.length());
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::AndroidLogPacket::Stats::kNumFailedFieldNumber:
        context_->storage->SetStats(stats::android_log_num_failed,
                                    fld.as_int64());
        break;
      case protos::AndroidLogPacket::Stats::kNumSkippedFieldNumber:
        context_->storage->SetStats(stats::android_log_num_skipped,
                                    fld.as_int64());
        break;
      case protos::AndroidLogPacket::Stats::kNumTotalFieldNumber:
        context_->storage->SetStats(stats::android_log_num_total,
                                    fld.as_int64());
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseTraceStats(TraceBlobView packet) {
  ProtoDecoder decoder(packet.data(), packet.length());
  int buf_num = 0;
  auto* storage = context_->storage.get();
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::TraceStats::kProducersConnectedFieldNumber:
        storage->SetStats(stats::traced_producers_connected, fld.as_int64());
        break;
      case protos::TraceStats::kProducersSeenFieldNumber:
        storage->SetStats(stats::traced_producers_seen, fld.as_int64());
        break;
      case protos::TraceStats::kDataSourcesRegisteredFieldNumber:
        storage->SetStats(stats::traced_data_sources_registered,
                          fld.as_int64());
        break;
      case protos::TraceStats::kDataSourcesSeenFieldNumber:
        storage->SetStats(stats::traced_data_sources_seen, fld.as_int64());
        break;
      case protos::TraceStats::kTracingSessionsFieldNumber:
        storage->SetStats(stats::traced_tracing_sessions, fld.as_int64());
        break;
      case protos::TraceStats::kTotalBuffersFieldNumber:
        storage->SetStats(stats::traced_total_buffers, fld.as_int64());
        break;
      case protos::TraceStats::kChunksDiscardedFieldNumber:
        storage->SetStats(stats::traced_chunks_discarded, fld.as_int64());
        break;
      case protos::TraceStats::kPatchesDiscardedFieldNumber:
        storage->SetStats(stats::traced_patches_discarded, fld.as_int64());
        break;
      case protos::TraceStats::kBufferStatsFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        TraceBlobView buf_data = packet.slice(fld_off, fld.size());
        ProtoDecoder buf_d(buf_data.data(), buf_data.length());
        for (auto fld2 = buf_d.ReadField(); fld2.id; fld2 = buf_d.ReadField()) {
          switch (fld2.id) {
            case protos::TraceStats::BufferStats::kBufferSizeFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_buffer_size, buf_num,
                                       fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::kBytesWrittenFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_bytes_written, buf_num,
                                       fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::kBytesOverwrittenFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_bytes_overwritten,
                                       buf_num, fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::kBytesReadFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_bytes_read, buf_num,
                                       fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::
                kPaddingBytesWrittenFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_padding_bytes_written,
                                       buf_num, fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::
                kPaddingBytesClearedFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_padding_bytes_cleared,
                                       buf_num, fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::kChunksWrittenFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_chunks_written,
                                       buf_num, fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::kChunksRewrittenFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_chunks_rewritten,
                                       buf_num, fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::kChunksOverwrittenFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_chunks_overwritten,
                                       buf_num, fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::kChunksDiscardedFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_chunks_discarded,
                                       buf_num, fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::kChunksReadFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_chunks_read, buf_num,
                                       fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::
                kChunksCommittedOutOfOrderFieldNumber:
              storage->SetIndexedStats(
                  stats::traced_buf_chunks_committed_out_of_order, buf_num,
                  fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::kWriteWrapCountFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_write_wrap_count,
                                       buf_num, fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::kPatchesSucceededFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_patches_succeeded,
                                       buf_num, fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::kPatchesFailedFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_patches_failed,
                                       buf_num, fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::
                kReadaheadsSucceededFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_readaheads_succeeded,
                                       buf_num, fld2.as_int64());
              break;
            case protos::TraceStats::BufferStats::kReadaheadsFailedFieldNumber:
              storage->SetIndexedStats(stats::traced_buf_readaheads_failed,
                                       buf_num, fld2.as_int64());
              break;
          }
        }  // for (buf_fld)
        buf_num++;
        break;
      }
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseFtraceStats(TraceBlobView packet) {
  ProtoDecoder decoder(packet.data(), packet.length());
  size_t phase = 0;
  auto* storage = context_->storage.get();
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::FtraceStats::kPhaseFieldNumber:
        phase = fld.int_value == protos::FtraceStats_Phase_END_OF_TRACE ? 1 : 0;

        // This code relies on the fact that each ftrace_cpu_XXX_end event is
        // just after the corresponding ftrace_cpu_XXX_begin event.
        static_assert(stats::ftrace_cpu_read_events_end -
                                  stats::ftrace_cpu_read_events_begin ==
                              1 &&
                          stats::ftrace_cpu_entries_end -
                                  stats::ftrace_cpu_entries_begin ==
                              1,
                      "ftrace_cpu_XXX stats definition are messed up");
        break;
      case protos::FtraceStats::kCpuStatsFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        TraceBlobView cpu_data = packet.slice(fld_off, fld.size());
        ProtoDecoder cpu_d(cpu_data.data(), cpu_data.length());
        int cpu_num = -1;
        for (auto fld2 = cpu_d.ReadField(); fld2.id; fld2 = cpu_d.ReadField()) {
          switch (fld2.id) {
            case protos::FtraceCpuStats::kCpuFieldNumber:
              cpu_num = fld2.as_int32();
              break;
            case protos::FtraceCpuStats::kEntriesFieldNumber:
              storage->SetIndexedStats(stats::ftrace_cpu_entries_begin + phase,
                                       cpu_num, fld2.as_int64());
              break;
            case protos::FtraceCpuStats::kOverrunFieldNumber:
              storage->SetIndexedStats(stats::ftrace_cpu_overrun_begin + phase,
                                       cpu_num, fld2.as_int64());
              break;
            case protos::FtraceCpuStats::kCommitOverrunFieldNumber:
              storage->SetIndexedStats(
                  stats::ftrace_cpu_commit_overrun_begin + phase, cpu_num,
                  fld2.as_int64());
              break;
            case protos::FtraceCpuStats::kBytesReadFieldNumber:
              storage->SetIndexedStats(
                  stats::ftrace_cpu_bytes_read_begin + phase, cpu_num,
                  fld2.as_int64());
              break;
            case protos::FtraceCpuStats::kOldestEventTsFieldNumber:
              storage->SetIndexedStats(
                  stats::ftrace_cpu_oldest_event_ts_begin + phase, cpu_num,
                  static_cast<int64_t>(fld2.as_double() * 1e9));
              break;
            case protos::FtraceCpuStats::kNowTsFieldNumber:
              storage->SetIndexedStats(
                  stats::ftrace_cpu_now_ts_begin + phase, cpu_num,
                  static_cast<int64_t>(fld2.as_double() * 1e9));
              break;
            case protos::FtraceCpuStats::kDroppedEventsFieldNumber:
              storage->SetIndexedStats(
                  stats::ftrace_cpu_dropped_events_begin + phase, cpu_num,
                  fld2.as_int64());
              break;
            case protos::FtraceCpuStats::kReadEventsFieldNumber:
              storage->SetIndexedStats(
                  stats::ftrace_cpu_read_events_begin + phase, cpu_num,
                  fld2.as_int64());
              break;
          }
        }  // for (buf_fld)
        break;
      }
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseProfilePacket(TraceBlobView packet) {
  ProtoDecoder decoder(packet.data(), packet.length());
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::ProfilePacket::kStringsFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        TraceBlobView nestedPacket = packet.slice(fld_off, fld.size());
        ProtoDecoder nested(nestedPacket.data(), nestedPacket.length());
        for (auto sub = nested.ReadField(); sub.id != 0;
             sub = nested.ReadField()) {
          switch (sub.id) {
            case protos::ProfilePacket::InternedString::kIdFieldNumber: {
              break;
            }
            case protos::ProfilePacket::InternedString::kStrFieldNumber: {
              context_->storage->InternString(sub.as_string());
              break;
            }
          }
        }
        break;
      }
      case protos::ProfilePacket::kFramesFieldNumber: {
        break;
      }
      case protos::ProfilePacket::kCallstacksFieldNumber: {
        break;
      }
      case protos::ProfilePacket::kMappingsFieldNumber: {
        break;
      }
      case protos::ProfilePacket::kProcessDumpsFieldNumber: {
        break;
      }
      case protos::ProfilePacket::kContinuedFieldNumber: {
        bool continued = fld.as_bool();
        base::ignore_result(continued);
        break;
      }
      case protos::ProfilePacket::kIndexFieldNumber: {
        int64_t index = fld.as_int64();
        base::ignore_result(index);
        break;
      }
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

}  // namespace trace_processor
}  // namespace perfetto
