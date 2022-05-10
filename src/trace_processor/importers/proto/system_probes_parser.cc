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

#include "src/trace_processor/importers/proto/system_probes_parser.h"

#include <set>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/traced/sys_stats_counters.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/system_info_tracker.h"
#include "src/trace_processor/importers/proto/metadata_tracker.h"
#include "src/trace_processor/importers/syscalls/syscall_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/ps/process_stats.pbzero.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/sys_stats/sys_stats.pbzero.h"
#include "protos/perfetto/trace/system_info.pbzero.h"
#include "protos/perfetto/trace/system_info/cpu_info.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {

base::Optional<int> VersionStringToSdkVersion(const std::string& version) {
  // TODO(lalitm): remove this when the SDK version polling saturates
  // S/T traces in practice.
  if (base::StartsWith(version, "T") || base::StartsWith(version, "S")) {
    return 31;
  }

  // Documentation for this mapping can be found at
  // https://source.android.com/compatibility/cdd.
  if (version == "12") {
    return 31;
  } else if (version == "11") {
    return 30;
  } else if (version == "10") {
    return 29;
  } else if (version == "9") {
    return 28;
  } else if (version == "8.1") {
    return 27;
  } else if (version == "8.0") {
    return 26;
  } else if (version == "7.1") {
    return 25;
  } else if (version == "7.0") {
    return 24;
  } else if (version == "6.0") {
    return 23;
  } else if (version == "5.1" || version == "5.1.1") {
    return 22;
  } else if (version == "5.0" || version == "5.0.1" || version == "5.0.2") {
    return 21;
  }
  // If we reached this point, we don't know how to parse this version
  // so just return null.
  return base::nullopt;
}

base::Optional<int> FingerprintToSdkVersion(const std::string& fingerprint) {
  // Try to parse the SDK version from the fingerprint.
  // Examples of fingerprints:
  // google/shamu/shamu:7.0/NBD92F/3753956:userdebug/dev-keys
  // google/coral/coral:12/SP1A.210812.015/7679548:userdebug/dev-keys
  size_t colon = fingerprint.find(':');
  if (colon == std::string::npos)
    return base::nullopt;

  size_t slash = fingerprint.find('/', colon);
  if (slash == std::string::npos)
    return base::nullopt;

  std::string version = fingerprint.substr(colon + 1, slash - (colon + 1));
  return VersionStringToSdkVersion(version);
}
}  // namespace

SystemProbesParser::SystemProbesParser(TraceProcessorContext* context)
    : context_(context),
      utid_name_id_(context->storage->InternString("utid")),
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
      oom_score_adj_id_(context->storage->InternString("oom_score_adj")),
      cpu_freq_id_(context_->storage->InternString("freq")) {
  for (const auto& name : BuildMeminfoCounterNames()) {
    meminfo_strs_id_.emplace_back(context->storage->InternString(name));
  }
  for (const auto& name : BuildVmstatCounterNames()) {
    vmstat_strs_id_.emplace_back(context->storage->InternString(name));
  }

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
}

void SystemProbesParser::ParseSysStats(int64_t ts, ConstBytes blob) {
  protos::pbzero::SysStats::Decoder sys_stats(blob.data, blob.size);

  for (auto it = sys_stats.meminfo(); it; ++it) {
    protos::pbzero::SysStats::MeminfoValue::Decoder mi(*it);
    auto key = static_cast<size_t>(mi.key());
    if (PERFETTO_UNLIKELY(key >= meminfo_strs_id_.size())) {
      PERFETTO_ELOG("MemInfo key %zu is not recognized.", key);
      context_->storage->IncrementStats(stats::meminfo_unknown_keys);
      continue;
    }
    // /proc/meminfo counters are in kB, convert to bytes
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        meminfo_strs_id_[key]);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(mi.value()) * 1024., track);
  }

  for (auto it = sys_stats.devfreq(); it; ++it) {
    protos::pbzero::SysStats::DevfreqValue::Decoder vm(*it);
    auto key = static_cast<base::StringView>(vm.key());
    // Append " Frequency" to align names with
    // FtraceParser::ParseClockSetRate
    base::StringView devfreq_subtitle("Frequency");
    base::StackString<255> counter_name(
        "%.*s %.*s", int(key.size()), key.data(), int(devfreq_subtitle.size()),
        devfreq_subtitle.data());
    StringId name = context_->storage->InternString(counter_name.string_view());
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(name);
    context_->event_tracker->PushCounter(ts, static_cast<double>(vm.value()),
                                         track);
  }

  int c = 0;
  for (auto it = sys_stats.cpufreq_khz(); it; ++it, ++c) {
    base::StackString<255> counter_name("CPU %d Freq in kHz", c);
    StringId name = context_->storage->InternString(counter_name.string_view());
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(name);
    context_->event_tracker->PushCounter(ts, static_cast<double>(*it), track);
  }

  for (auto it = sys_stats.vmstat(); it; ++it) {
    protos::pbzero::SysStats::VmstatValue::Decoder vm(*it);
    auto key = static_cast<size_t>(vm.key());
    if (PERFETTO_UNLIKELY(key >= vmstat_strs_id_.size())) {
      PERFETTO_ELOG("VmStat key %zu is not recognized.", key);
      context_->storage->IncrementStats(stats::vmstat_unknown_keys);
      continue;
    }
    TrackId track =
        context_->track_tracker->InternGlobalCounterTrack(vmstat_strs_id_[key]);
    context_->event_tracker->PushCounter(ts, static_cast<double>(vm.value()),
                                         track);
  }

  for (auto it = sys_stats.cpu_stat(); it; ++it) {
    protos::pbzero::SysStats::CpuTimes::Decoder ct(*it);
    if (PERFETTO_UNLIKELY(!ct.has_cpu_id())) {
      PERFETTO_ELOG("CPU field not found in CpuTimes");
      context_->storage->IncrementStats(stats::invalid_cpu_times);
      continue;
    }

    TrackId track = context_->track_tracker->InternCpuCounterTrack(
        cpu_times_user_ns_id_, ct.cpu_id());
    context_->event_tracker->PushCounter(ts, static_cast<double>(ct.user_ns()),
                                         track);

    track = context_->track_tracker->InternCpuCounterTrack(
        cpu_times_user_nice_ns_id_, ct.cpu_id());
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(ct.user_ice_ns()), track);

    track = context_->track_tracker->InternCpuCounterTrack(
        cpu_times_system_mode_ns_id_, ct.cpu_id());
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(ct.system_mode_ns()), track);

    track = context_->track_tracker->InternCpuCounterTrack(
        cpu_times_idle_ns_id_, ct.cpu_id());
    context_->event_tracker->PushCounter(ts, static_cast<double>(ct.idle_ns()),
                                         track);

    track = context_->track_tracker->InternCpuCounterTrack(
        cpu_times_io_wait_ns_id_, ct.cpu_id());
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(ct.io_wait_ns()), track);

    track = context_->track_tracker->InternCpuCounterTrack(cpu_times_irq_ns_id_,
                                                           ct.cpu_id());
    context_->event_tracker->PushCounter(ts, static_cast<double>(ct.irq_ns()),
                                         track);

    track = context_->track_tracker->InternCpuCounterTrack(
        cpu_times_softirq_ns_id_, ct.cpu_id());
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(ct.softirq_ns()), track);
  }

  for (auto it = sys_stats.num_irq(); it; ++it) {
    protos::pbzero::SysStats::InterruptCount::Decoder ic(*it);

    TrackId track = context_->track_tracker->InternIrqCounterTrack(
        num_irq_name_id_, ic.irq());
    context_->event_tracker->PushCounter(ts, static_cast<double>(ic.count()),
                                         track);
  }

  for (auto it = sys_stats.num_softirq(); it; ++it) {
    protos::pbzero::SysStats::InterruptCount::Decoder ic(*it);

    TrackId track = context_->track_tracker->InternSoftirqCounterTrack(
        num_softirq_name_id_, ic.irq());
    context_->event_tracker->PushCounter(ts, static_cast<double>(ic.count()),
                                         track);
  }

  if (sys_stats.has_num_forks()) {
    TrackId track =
        context_->track_tracker->InternGlobalCounterTrack(num_forks_name_id_);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(sys_stats.num_forks()), track);
  }

  if (sys_stats.has_num_irq_total()) {
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        num_irq_total_name_id_);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(sys_stats.num_irq_total()), track);
  }

  if (sys_stats.has_num_softirq_total()) {
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        num_softirq_total_name_id_);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(sys_stats.num_softirq_total()), track);
  }
}

void SystemProbesParser::ParseProcessTree(ConstBytes blob) {
  protos::pbzero::ProcessTree::Decoder ps(blob.data, blob.size);

  for (auto it = ps.processes(); it; ++it) {
    protos::pbzero::ProcessTree::Process::Decoder proc(*it);
    if (!proc.has_cmdline())
      continue;
    auto pid = static_cast<uint32_t>(proc.pid());
    auto ppid = static_cast<uint32_t>(proc.ppid());

    if (proc.has_nspid()) {
      std::vector<uint32_t> nspid;
      for (auto nspid_it = proc.nspid(); nspid_it; nspid_it++) {
        nspid.emplace_back(static_cast<uint32_t>(*nspid_it));
      }
      context_->process_tracker->UpdateNamespacedProcess(pid, std::move(nspid));
    }

    auto raw_cmdline = proc.cmdline();
    base::StringView argv0 = raw_cmdline ? *raw_cmdline : base::StringView();
    // Chrome child process overwrites /proc/self/cmdline and replaces all
    // '\0' with ' '. This makes argv0 contain the full command line. Extract
    // the actual argv0 if it's Chrome.
    static const char kChromeBinary[] = "/chrome ";
    auto pos = argv0.find(kChromeBinary);
    if (pos != base::StringView::npos) {
      argv0 = argv0.substr(0, pos + strlen(kChromeBinary) - 1);
    }

    std::string cmdline_str;
    for (auto cmdline_it = raw_cmdline; cmdline_it;) {
      auto cmdline_part = *cmdline_it;
      cmdline_str.append(cmdline_part.data, cmdline_part.size);

      if (++cmdline_it)
        cmdline_str.append(" ");
    }
    base::StringView cmdline = base::StringView(cmdline_str);
    UniquePid upid = context_->process_tracker->SetProcessMetadata(
        pid, ppid, argv0, cmdline);
    if (proc.has_uid()) {
      context_->process_tracker->SetProcessUid(
          upid, static_cast<uint32_t>(proc.uid()));
    }
  }

  for (auto it = ps.threads(); it; ++it) {
    protos::pbzero::ProcessTree::Thread::Decoder thd(*it);
    auto tid = static_cast<uint32_t>(thd.tid());
    auto tgid = static_cast<uint32_t>(thd.tgid());
    context_->process_tracker->UpdateThread(tid, tgid);

    if (thd.has_name()) {
      StringId thread_name_id = context_->storage->InternString(thd.name());
      context_->process_tracker->UpdateThreadName(
          tid, thread_name_id, ThreadNamePriority::kProcessTree);
    }

    if (thd.has_nstid()) {
      std::vector<uint32_t> nstid;
      for (auto nstid_it = thd.nstid(); nstid_it; nstid_it++) {
        nstid.emplace_back(static_cast<uint32_t>(*nstid_it));
      }
      context_->process_tracker->UpdateNamespacedThread(tgid, tid,
                                                        std::move(nstid));
    }
  }
}

void SystemProbesParser::ParseProcessStats(int64_t ts, ConstBytes blob) {
  using Process = protos::pbzero::ProcessStats::Process;
  protos::pbzero::ProcessStats::Decoder stats(blob.data, blob.size);
  const auto kOomScoreAdjFieldNumber =
      protos::pbzero::ProcessStats::Process::kOomScoreAdjFieldNumber;
  for (auto it = stats.processes(); it; ++it) {
    // Maps a process counter field it to its value.
    // E.g., 4 := 1024 -> "mem.rss.anon" := 1024.
    std::array<int64_t, kProcStatsProcessSize> counter_values{};
    std::array<bool, kProcStatsProcessSize> has_counter{};

    protozero::ProtoDecoder proc(*it);
    uint32_t pid = 0;
    for (auto fld = proc.ReadField(); fld.valid(); fld = proc.ReadField()) {
      if (fld.id() == protos::pbzero::ProcessStats::Process::kPidFieldNumber) {
        pid = fld.as_uint32();
        continue;
      }
      if (fld.id() ==
          protos::pbzero::ProcessStats::Process::kThreadsFieldNumber) {
        ParseThreadStats(ts, pid, fld.as_bytes());
        continue;
      }
      bool is_counter_field = fld.id() < proc_stats_process_names_.size() &&
                              !proc_stats_process_names_[fld.id()].is_null();
      if (is_counter_field) {
        // Memory counters are in KB, keep values in bytes in the trace
        // processor.
        counter_values[fld.id()] = fld.id() == kOomScoreAdjFieldNumber
                                       ? fld.as_int64()
                                       : fld.as_int64() * 1024;
        has_counter[fld.id()] = true;
      } else {
        // Chrome fields are processed by ChromeSystemProbesParser.
        if (fld.id() == Process::kIsPeakRssResettableFieldNumber ||
            fld.id() == Process::kChromePrivateFootprintKbFieldNumber ||
            fld.id() == Process::kChromePrivateFootprintKbFieldNumber) {
          continue;
        }
        context_->storage->IncrementStats(stats::proc_stat_unknown_counters);
      }
    }

    // Skip field_id 0 (invalid) and 1 (pid).
    for (size_t field_id = 2; field_id < counter_values.size(); field_id++) {
      if (!has_counter[field_id] || field_id ==
                                        protos::pbzero::ProcessStats::Process::
                                            kIsPeakRssResettableFieldNumber) {
        continue;
      }

      // Lookup the interned string id from the field name using the
      // pre-cached |proc_stats_process_names_| map.
      const StringId& name = proc_stats_process_names_[field_id];
      UniquePid upid = context_->process_tracker->GetOrCreateProcess(pid);
      TrackId track =
          context_->track_tracker->InternProcessCounterTrack(name, upid);
      int64_t value = counter_values[field_id];
      context_->event_tracker->PushCounter(ts, static_cast<double>(value),
                                           track);
    }
  }
}

void SystemProbesParser::ParseThreadStats(int64_t,
                                          uint32_t pid,
                                          ConstBytes blob) {
  protos::pbzero::ProcessStats::Thread::Decoder stats(blob.data, blob.size);
  context_->process_tracker->UpdateThread(static_cast<uint32_t>(stats.tid()),
                                          pid);
}

void SystemProbesParser::ParseSystemInfo(ConstBytes blob) {
  protos::pbzero::SystemInfo::Decoder packet(blob.data, blob.size);
  if (packet.has_utsname()) {
    ConstBytes utsname_blob = packet.utsname();
    protos::pbzero::Utsname::Decoder utsname(utsname_blob.data,
                                             utsname_blob.size);
    base::StringView machine = utsname.machine();
    SyscallTracker* syscall_tracker = SyscallTracker::GetOrCreate(context_);
    if (machine == "aarch64") {
      syscall_tracker->SetArchitecture(kAarch64);
    } else if (machine == "armv8l") {
      syscall_tracker->SetArchitecture(kArmEabi);
    } else if (machine == "armv7l") {
      syscall_tracker->SetArchitecture(kAarch32);
    } else if (machine == "x86_64") {
      syscall_tracker->SetArchitecture(kX86_64);
    } else if (machine == "i686") {
      syscall_tracker->SetArchitecture(kX86);
    } else {
      PERFETTO_ELOG("Unknown architecture %s. Syscall traces will not work.",
                    machine.ToStdString().c_str());
    }

    SystemInfoTracker* system_info_tracker =
        SystemInfoTracker::GetOrCreate(context_);
    system_info_tracker->SetKernelVersion(utsname.sysname(), utsname.release());

    StringPool::Id sysname_id =
        context_->storage->InternString(utsname.sysname());
    StringPool::Id version_id =
        context_->storage->InternString(utsname.version());
    StringPool::Id release_id =
        context_->storage->InternString(utsname.release());
    StringPool::Id machine_id =
        context_->storage->InternString(utsname.machine());

    MetadataTracker* metadata = context_->metadata_tracker.get();
    metadata->SetMetadata(metadata::system_name, Variadic::String(sysname_id));
    metadata->SetMetadata(metadata::system_version,
                          Variadic::String(version_id));
    metadata->SetMetadata(metadata::system_release,
                          Variadic::String(release_id));
    metadata->SetMetadata(metadata::system_machine,
                          Variadic::String(machine_id));
  }

  if (packet.has_android_build_fingerprint()) {
    context_->metadata_tracker->SetMetadata(
        metadata::android_build_fingerprint,
        Variadic::String(context_->storage->InternString(
            packet.android_build_fingerprint())));
  }

  // If we have the SDK version in the trace directly just use that.
  // Otherwise, try and parse it from the fingerprint.
  base::Optional<int64_t> opt_sdk_version;
  if (packet.has_android_sdk_version()) {
    opt_sdk_version = static_cast<int64_t>(packet.android_sdk_version());
  } else if (packet.has_android_build_fingerprint()) {
    opt_sdk_version = FingerprintToSdkVersion(
        packet.android_build_fingerprint().ToStdString());
  }

  if (opt_sdk_version) {
    context_->metadata_tracker->SetMetadata(
        metadata::android_sdk_version, Variadic::Integer(*opt_sdk_version));
  }

  int64_t hz = packet.hz();
  if (hz > 0)
    ms_per_tick_ = 1000u / static_cast<uint64_t>(hz);
}

void SystemProbesParser::ParseCpuInfo(ConstBytes blob) {
  protos::pbzero::CpuInfo::Decoder packet(blob.data, blob.size);
  uint32_t cluster_id = 0;
  std::vector<uint32_t> last_cpu_freqs;
  uint32_t cpu_index = 0;
  for (auto it = packet.cpus(); it; it++, cpu_index++) {
    protos::pbzero::CpuInfo::Cpu::Decoder cpu(*it);
    tables::CpuTable::Row cpu_row;
    if (cpu.has_processor()) {
      cpu_row.processor = context_->storage->InternString(cpu.processor());
    }
    std::vector<uint32_t> freqs;
    for (auto freq_it = cpu.frequencies(); freq_it; freq_it++) {
      freqs.push_back(*freq_it);
    }

    // Here we assume that cluster of CPUs are 'next' to each other.
    if (freqs != last_cpu_freqs) {
      cluster_id = cpu_index;
    }
    cpu_row.cluster_id = cluster_id;
    cpu_row.time_in_state_cpu_id = cluster_id;
    last_cpu_freqs = freqs;
    tables::CpuTable::Id cpu_row_id =
        context_->storage->mutable_cpu_table()->Insert(cpu_row).id;

    for (auto freq_it = cpu.frequencies(); freq_it; freq_it++) {
      uint32_t freq = *freq_it;
      tables::CpuFreqTable::Row cpu_freq_row;
      cpu_freq_row.cpu_id = cpu_row_id;
      cpu_freq_row.freq = freq;
      context_->storage->mutable_cpu_freq_table()->Insert(cpu_freq_row);
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
