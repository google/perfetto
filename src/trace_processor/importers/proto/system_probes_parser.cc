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

#include <cstdint>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/traced/sys_stats_counters.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/system_info_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/syscalls/syscall_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/ps/process_stats.pbzero.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/system_info.pbzero.h"
#include "protos/perfetto/trace/system_info/cpu_info.pbzero.h"

namespace {

bool IsSupportedDiskStatDevice(const std::string& device_name) {
  return device_name == "sda";  // Primary SCSI disk device name
}

}  // namespace

namespace perfetto {
namespace trace_processor {

namespace {

std::optional<int> VersionStringToSdkVersion(const std::string& version) {
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
  return std::nullopt;
}

std::optional<int> FingerprintToSdkVersion(const std::string& fingerprint) {
  // Try to parse the SDK version from the fingerprint.
  // Examples of fingerprints:
  // google/shamu/shamu:7.0/NBD92F/3753956:userdebug/dev-keys
  // google/coral/coral:12/SP1A.210812.015/7679548:userdebug/dev-keys
  size_t colon = fingerprint.find(':');
  if (colon == std::string::npos)
    return std::nullopt;

  size_t slash = fingerprint.find('/', colon);
  if (slash == std::string::npos)
    return std::nullopt;

  std::string version = fingerprint.substr(colon + 1, slash - (colon + 1));
  return VersionStringToSdkVersion(version);
}

struct ArmCpuIdentifier {
  uint32_t implementer;
  uint32_t architecture;
  uint32_t variant;
  uint32_t part;
  uint32_t revision;
};

struct CpuInfo {
  uint32_t cpu = 0;
  std::optional<uint32_t> capacity;
  std::vector<uint32_t> frequencies;
  protozero::ConstChars processor;
  // Extend the variant to support additional identifiers
  std::variant<std::nullopt_t, ArmCpuIdentifier> identifier = std::nullopt;
};

struct CpuMaxFrequency {
  uint32_t cpu = 0;
  uint32_t max_frequency = 0;
};

}  // namespace

SystemProbesParser::SystemProbesParser(TraceProcessorContext* context)
    : context_(context),
      utid_name_id_(context->storage->InternString("utid")),
      ns_unit_id_(context->storage->InternString("ns")),
      bytes_unit_id_(context->storage->InternString("bytes")),
      available_chunks_unit_id_(
          context->storage->InternString("available chunks")),
      num_forks_name_id_(context->storage->InternString("num_forks")),
      num_irq_total_name_id_(context->storage->InternString("num_irq_total")),
      num_softirq_total_name_id_(
          context->storage->InternString("num_softirq_total")),
      oom_score_adj_id_(context->storage->InternString("oom_score_adj")),
      thermal_unit_id_(context->storage->InternString("C")),
      gpufreq_id(context->storage->InternString("gpufreq")),
      gpufreq_unit_id(context->storage->InternString("MHz")),
      arm_cpu_implementer(
          context->storage->InternString("arm_cpu_implementer")),
      arm_cpu_architecture(
          context->storage->InternString("arm_cpu_architecture")),
      arm_cpu_variant(context->storage->InternString("arm_cpu_variant")),
      arm_cpu_part(context->storage->InternString("arm_cpu_part")),
      arm_cpu_revision(context->storage->InternString("arm_cpu_revision")) {
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
  proc_stats_process_names_[ProcessStats::Process::kSmrRssKbFieldNumber] =
      context->storage->InternString("mem.smaps.rss");
  proc_stats_process_names_[ProcessStats::Process::kSmrPssKbFieldNumber] =
      context->storage->InternString("mem.smaps.pss");
  proc_stats_process_names_[ProcessStats::Process::kSmrPssAnonKbFieldNumber] =
      context->storage->InternString("mem.smaps.pss.anon");
  proc_stats_process_names_[ProcessStats::Process::kSmrPssFileKbFieldNumber] =
      context->storage->InternString("mem.smaps.pss.file");
  proc_stats_process_names_[ProcessStats::Process::kSmrPssShmemKbFieldNumber] =
      context->storage->InternString("mem.smaps.pss.shmem");
  proc_stats_process_names_[ProcessStats::Process::kSmrSwapPssKbFieldNumber] =
      context->storage->InternString("mem.smaps.swap.pss");
  proc_stats_process_names_
      [ProcessStats::Process::kRuntimeUserModeFieldNumber] =
          context->storage->InternString("runtime.user_ns");
  proc_stats_process_names_
      [ProcessStats::Process::kRuntimeKernelModeFieldNumber] =
          context->storage->InternString("runtime.kernel_ns");

  using PsiResource = protos::pbzero::SysStats::PsiSample::PsiResource;
  sys_stats_psi_resource_names_[PsiResource::PSI_RESOURCE_UNSPECIFIED] =
      context->storage->InternString("psi.resource.unspecified");
  sys_stats_psi_resource_names_[PsiResource::PSI_RESOURCE_CPU_SOME] =
      context->storage->InternString("psi.cpu.some");
  sys_stats_psi_resource_names_[PsiResource::PSI_RESOURCE_CPU_FULL] =
      context->storage->InternString("psi.cpu.full");
  sys_stats_psi_resource_names_[PsiResource::PSI_RESOURCE_IO_SOME] =
      context->storage->InternString("psi.io.some");
  sys_stats_psi_resource_names_[PsiResource::PSI_RESOURCE_IO_FULL] =
      context->storage->InternString("psi.io.full");
  sys_stats_psi_resource_names_[PsiResource::PSI_RESOURCE_MEMORY_SOME] =
      context->storage->InternString("psi.mem.some");
  sys_stats_psi_resource_names_[PsiResource::PSI_RESOURCE_MEMORY_FULL] =
      context->storage->InternString("psi.mem.full");
}

void SystemProbesParser::ParseDiskStats(int64_t ts, ConstBytes blob) {
  protos::pbzero::SysStats::DiskStat::Decoder ds(blob.data, blob.size);
  static constexpr double SECTORS_PER_MB = 2048.0;
  static constexpr double MS_PER_SEC = 1000.0;
  std::string device_name = ds.device_name().ToStdString();
  if (!IsSupportedDiskStatDevice(device_name)) {
    return;
  }

  base::StackString<512> tag_prefix("diskstat.[%s]", device_name.c_str());
  auto push_counter = [this, ts, tag_prefix](const char* counter_name,
                                             double value) {
    base::StackString<512> track_name("%s.%s", tag_prefix.c_str(),
                                      counter_name);
    StringId string_id = context_->storage->InternString(track_name.c_str());
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        TrackTracker::Group::kIo, string_id);
    context_->event_tracker->PushCounter(ts, value, track);
  };

  // TODO(rsavitski): with the UI now supporting rate mode for counter tracks,
  // this is likely redundant.
  auto calculate_throughput = [](double amount, int64_t diff) {
    return diff == 0 ? 0 : amount * MS_PER_SEC / static_cast<double>(diff);
  };

  int64_t cur_read_amount = static_cast<int64_t>(ds.read_sectors());
  int64_t cur_write_amount = static_cast<int64_t>(ds.write_sectors());
  int64_t cur_discard_amount = static_cast<int64_t>(ds.discard_sectors());
  int64_t cur_flush_count = static_cast<int64_t>(ds.flush_count());
  int64_t cur_read_time = static_cast<int64_t>(ds.read_time_ms());
  int64_t cur_write_time = static_cast<int64_t>(ds.write_time_ms());
  int64_t cur_discard_time = static_cast<int64_t>(ds.discard_time_ms());
  int64_t cur_flush_time = static_cast<int64_t>(ds.flush_time_ms());

  if (prev_read_amount != -1) {
    double read_amount =
        static_cast<double>(cur_read_amount - prev_read_amount) /
        SECTORS_PER_MB;
    double write_amount =
        static_cast<double>(cur_write_amount - prev_write_amount) /
        SECTORS_PER_MB;
    double discard_amount =
        static_cast<double>(cur_discard_amount - prev_discard_amount) /
        SECTORS_PER_MB;
    double flush_count =
        static_cast<double>(cur_flush_count - prev_flush_count);
    int64_t read_time_diff = cur_read_time - prev_read_time;
    int64_t write_time_diff = cur_write_time - prev_write_time;
    int64_t discard_time_diff = cur_discard_time - prev_discard_time;
    double flush_time_diff =
        static_cast<double>(cur_flush_time - prev_flush_time);

    double read_thpt = calculate_throughput(read_amount, read_time_diff);
    double write_thpt = calculate_throughput(write_amount, write_time_diff);
    double discard_thpt =
        calculate_throughput(discard_amount, discard_time_diff);

    push_counter("read_amount(mg)", read_amount);
    push_counter("read_throughput(mg/s)", read_thpt);
    push_counter("write_amount(mg)", write_amount);
    push_counter("write_throughput(mg/s)", write_thpt);
    push_counter("discard_amount(mg)", discard_amount);
    push_counter("discard_throughput(mg/s)", discard_thpt);
    push_counter("flush_amount(count)", flush_count);
    push_counter("flush_time(ms)", flush_time_diff);
  }

  prev_read_amount = cur_read_amount;
  prev_write_amount = cur_write_amount;
  prev_discard_amount = cur_discard_amount;
  prev_flush_count = cur_flush_count;
  prev_read_time = cur_read_time;
  prev_write_time = cur_write_time;
  prev_discard_time = cur_discard_time;
  prev_flush_time = cur_flush_time;
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
        TrackTracker::Group::kMemory, meminfo_strs_id_[key], {},
        bytes_unit_id_);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(mi.value()) * 1024, track);
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
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        TrackTracker::Group::kClockFrequency, name);
    context_->event_tracker->PushCounter(ts, static_cast<double>(vm.value()),
                                         track);
  }

  uint32_t c = 0;
  for (auto it = sys_stats.cpufreq_khz(); it; ++it, ++c) {
    TrackId track = context_->track_tracker->InternCpuCounterTrack(
        TrackTracker::TrackClassification::kCpuFrequency, c);
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
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        TrackTracker::Group::kMemory, vmstat_strs_id_[key]);
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
        TrackTracker::TrackClassification::kUserTime, ct.cpu_id());
    context_->event_tracker->PushCounter(ts, static_cast<double>(ct.user_ns()),
                                         track);

    track = context_->track_tracker->InternCpuCounterTrack(
        TrackTracker::TrackClassification::kNiceUserTime, ct.cpu_id());
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(ct.user_nice_ns()), track);

    track = context_->track_tracker->InternCpuCounterTrack(
        TrackTracker::TrackClassification::kSystemModeTime, ct.cpu_id());
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(ct.system_mode_ns()), track);

    track = context_->track_tracker->InternCpuCounterTrack(
        TrackTracker::TrackClassification::kCpuIdleTime, ct.cpu_id());
    context_->event_tracker->PushCounter(ts, static_cast<double>(ct.idle_ns()),
                                         track);

    track = context_->track_tracker->InternCpuCounterTrack(
        TrackTracker::TrackClassification::kIoWaitTime, ct.cpu_id());
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(ct.io_wait_ns()), track);

    track = context_->track_tracker->InternCpuCounterTrack(
        TrackTracker::TrackClassification::kIrqTime, ct.cpu_id());
    context_->event_tracker->PushCounter(ts, static_cast<double>(ct.irq_ns()),
                                         track);

    track = context_->track_tracker->InternCpuCounterTrack(
        TrackTracker::TrackClassification::kSoftIrqTime, ct.cpu_id());
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(ct.softirq_ns()), track);
  }

  for (auto it = sys_stats.num_irq(); it; ++it) {
    protos::pbzero::SysStats::InterruptCount::Decoder ic(*it);

    TrackId track = context_->track_tracker->LegacyInternIrqCounterTrack(
        TrackTracker::TrackClassification::kIrqCount, ic.irq());
    context_->event_tracker->PushCounter(ts, static_cast<double>(ic.count()),
                                         track);
  }

  for (auto it = sys_stats.num_softirq(); it; ++it) {
    protos::pbzero::SysStats::InterruptCount::Decoder ic(*it);

    TrackId track = context_->track_tracker->LegacyInternSoftirqCounterTrack(
        TrackTracker::TrackClassification::kSoftirqCount, ic.irq());
    context_->event_tracker->PushCounter(ts, static_cast<double>(ic.count()),
                                         track);
  }

  if (sys_stats.has_num_forks()) {
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        TrackTracker::Group::kDeviceState, num_forks_name_id_);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(sys_stats.num_forks()), track);
  }

  if (sys_stats.has_num_irq_total()) {
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        TrackTracker::Group::kDeviceState, num_irq_total_name_id_);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(sys_stats.num_irq_total()), track);
  }

  if (sys_stats.has_num_softirq_total()) {
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        TrackTracker::Group::kDeviceState, num_softirq_total_name_id_);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(sys_stats.num_softirq_total()), track);
  }

  // Fragmentation of the kernel binary buddy memory allocator.
  // See /proc/buddyinfo in `man 5 proc`.
  for (auto it = sys_stats.buddy_info(); it; ++it) {
    protos::pbzero::SysStats::BuddyInfo::Decoder bi(*it);
    int order = 0;
    for (auto order_it = bi.order_pages(); order_it; ++order_it) {
      std::string node = bi.node().ToStdString();
      std::string zone = bi.zone().ToStdString();
      uint32_t chunk_size_kb =
          static_cast<uint32_t>(((1 << order) * page_size_) / 1024);
      base::StackString<255> counter_name("mem.buddyinfo[%s][%s][%u kB]",
                                          node.c_str(), zone.c_str(),
                                          chunk_size_kb);
      StringId name =
          context_->storage->InternString(counter_name.string_view());
      TrackId track = context_->track_tracker->InternGlobalCounterTrack(
          TrackTracker::Group::kMemory, name, {}, available_chunks_unit_id_);
      context_->event_tracker->PushCounter(ts, static_cast<double>(*order_it),
                                           track);
      order++;
    }
  }

  for (auto it = sys_stats.disk_stat(); it; ++it) {
    ParseDiskStats(ts, *it);
  }

  // Pressure Stall Information. See
  // https://docs.kernel.org/accounting/psi.html.
  for (auto it = sys_stats.psi(); it; ++it) {
    protos::pbzero::SysStats::PsiSample::Decoder psi(*it);

    auto resource = static_cast<size_t>(psi.resource());
    if (PERFETTO_UNLIKELY(resource >= sys_stats_psi_resource_names_.size())) {
      PERFETTO_ELOG("PsiResource type %zu is not recognized.", resource);
      context_->storage->IncrementStats(stats::psi_unknown_resource);
      continue;
    }

    // Unit = total blocked time on this resource in nanoseconds.
    // TODO(b/315152880): Consider moving psi entries for cpu/io/memory into
    // groups specific to that resource (e.g., `Group::kMemory`).
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        TrackTracker::Group::kDeviceState,
        sys_stats_psi_resource_names_[resource], {}, ns_unit_id_);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(psi.total_ns()), track);
  }

  for (auto it = sys_stats.thermal_zone(); it; ++it) {
    protos::pbzero::SysStats::ThermalZone::Decoder thermal(*it);
    StringId track_name = context_->storage->InternString(thermal.type());
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        TrackTracker::Group::kThermals, track_name, {}, thermal_unit_id_);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(thermal.temp()), track);
  }

  for (auto it = sys_stats.cpuidle_state(); it; ++it) {
    ParseCpuIdleStats(ts, *it);
  }

  for (auto it = sys_stats.gpufreq_mhz(); it; ++it, ++c) {
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        TrackTracker::Group::kPower, gpufreq_id, {}, gpufreq_unit_id);
    context_->event_tracker->PushCounter(ts, static_cast<double>(*it), track);
  }
}

void SystemProbesParser::ParseCpuIdleStats(int64_t ts, ConstBytes blob) {
  protos::pbzero::SysStats::CpuIdleState::Decoder cpuidle_state(blob);
  uint32_t cpu_id = cpuidle_state.cpu_id();
  for (auto cpuidle_field = cpuidle_state.cpuidle_state_entry(); cpuidle_field;
       ++cpuidle_field) {
    protos::pbzero::SysStats::CpuIdleStateEntry::Decoder idle(*cpuidle_field);

    TrackId track = context_->track_tracker->LegacyInternCpuIdleStateTrack(
        cpu_id, context_->storage->InternString(idle.state()));
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(idle.duration_us()), track);
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

    protozero::RepeatedFieldIterator<protozero::ConstChars> raw_cmdline =
        proc.cmdline();
    base::StringView argv0 = raw_cmdline ? *raw_cmdline : base::StringView();
    base::StringView joined_cmdline{};

    // Special case: workqueue kernel threads (kworker). Worker threads are
    // organised in pools, which can process work from different workqueues.
    // When we read their thread name via procfs, the kernel takes a dedicated
    // codepath that appends the name of the current/last workqueue that the
    // worker processed. This is highly transient and therefore misleading to
    // users if we keep using this name for the kernel thread.
    // Example:
    //   kworker/45:2-mm_percpu_wq
    //   ^           ^
    //   [worker id ][last queue ]
    //
    // Instead, use a truncated version of the process name that identifies just
    // the worker itself. For the above example, this would be "kworker/45:2".
    //
    // https://github.com/torvalds/linux/blob/6d280f4d760e3bcb4a8df302afebf085b65ec982/kernel/workqueue.c#L5336
    uint32_t kThreaddPid = 2;
    if (ppid == kThreaddPid && argv0.StartsWith("kworker/")) {
      size_t delim_loc = std::min(argv0.find('+', 8), argv0.find('-', 8));
      if (delim_loc != base::StringView::npos) {
        argv0 = argv0.substr(0, delim_loc);
        joined_cmdline = argv0;
      }
    }

    // Special case: some processes rewrite their cmdline with spaces as a
    // separator instead of a NUL byte. Assume that's the case if there's only a
    // single cmdline element. This will be wrong for binaries that have spaces
    // in their path and are invoked without additional arguments, but those are
    // very rare. The full cmdline will still be correct either way.
    if (bool(++proc.cmdline()) == false) {
      size_t delim_pos = argv0.find(' ');
      if (delim_pos != base::StringView::npos) {
        argv0 = argv0.substr(0, delim_pos);
      }
    }

    std::string cmdline_str;
    if (joined_cmdline.empty()) {
      for (auto cmdline_it = raw_cmdline; cmdline_it;) {
        auto cmdline_part = *cmdline_it;
        cmdline_str.append(cmdline_part.data, cmdline_part.size);

        if (++cmdline_it)
          cmdline_str.append(" ");
      }
      joined_cmdline = base::StringView(cmdline_str);
    }
    UniquePid upid = context_->process_tracker->SetProcessMetadata(
        pid, ppid, argv0, joined_cmdline);

    if (proc.has_uid()) {
      context_->process_tracker->SetProcessUid(
          upid, static_cast<uint32_t>(proc.uid()));
    }

    // note: early kernel threads can have an age of zero (at tick resolution)
    if (proc.has_process_start_from_boot()) {
      base::StatusOr<int64_t> start_ts = context_->clock_tracker->ToTraceTime(
          protos::pbzero::BUILTIN_CLOCK_BOOTTIME,
          static_cast<int64_t>(proc.process_start_from_boot()));
      if (start_ts.ok()) {
        context_->process_tracker->SetStartTsIfUnset(upid, *start_ts);
      }
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
      if (fld.id() == protos::pbzero::ProcessStats::Process::kFdsFieldNumber) {
        ParseProcessFds(ts, pid, fld.as_bytes());
        continue;
      }
      bool is_counter_field = fld.id() < proc_stats_process_names_.size() &&
                              !proc_stats_process_names_[fld.id()].is_null();
      if (is_counter_field) {
        // Memory counters are in KB, keep values in bytes in the trace
        // processor.
        int64_t value = fld.as_int64();
        if (fld.id() != Process::kOomScoreAdjFieldNumber &&
            fld.id() != Process::kRuntimeUserModeFieldNumber &&
            fld.id() != Process::kRuntimeKernelModeFieldNumber) {
          value = value * 1024;  // KB -> B
        }
        counter_values[fld.id()] = value;
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

void SystemProbesParser::ParseProcessFds(int64_t ts,
                                         uint32_t pid,
                                         ConstBytes blob) {
  protos::pbzero::ProcessStats::FDInfo::Decoder fd_info(blob.data, blob.size);

  tables::FiledescriptorTable::Row row;
  row.fd = static_cast<int64_t>(fd_info.fd());
  row.ts = ts;
  row.path = context_->storage->InternString(fd_info.path());
  row.upid = context_->process_tracker->GetOrCreateProcess(pid);

  auto* fd_table = context_->storage->mutable_filedescriptor_table();
  fd_table->Insert(row);
}

void SystemProbesParser::ParseSystemInfo(ConstBytes blob) {
  protos::pbzero::SystemInfo::Decoder packet(blob.data, blob.size);
  SystemInfoTracker* system_info_tracker =
      SystemInfoTracker::GetOrCreate(context_);
  if (packet.has_utsname()) {
    ConstBytes utsname_blob = packet.utsname();
    protos::pbzero::Utsname::Decoder utsname(utsname_blob.data,
                                             utsname_blob.size);
    base::StringView machine = utsname.machine();
    SyscallTracker* syscall_tracker = SyscallTracker::GetOrCreate(context_);
    Architecture arch = SyscallTable::ArchFromString(machine);
    if (arch != Architecture::kUnknown) {
      syscall_tracker->SetArchitecture(arch);
    } else {
      PERFETTO_ELOG("Unknown architecture %s. Syscall traces will not work.",
                    machine.ToStdString().c_str());
    }

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

  if (packet.has_timezone_off_mins()) {
    static constexpr int64_t kNanosInMinute =
        60ull * 1000ull * 1000ull * 1000ull;
    context_->metadata_tracker->SetMetadata(
        metadata::timezone_off_mins,
        Variadic::Integer(packet.timezone_off_mins()));
    context_->clock_tracker->set_timezone_offset(packet.timezone_off_mins() *
                                                 kNanosInMinute);
  }

  if (packet.has_android_build_fingerprint()) {
    context_->metadata_tracker->SetMetadata(
        metadata::android_build_fingerprint,
        Variadic::String(context_->storage->InternString(
            packet.android_build_fingerprint())));
  }

  // If we have the SDK version in the trace directly just use that.
  // Otherwise, try and parse it from the fingerprint.
  std::optional<int64_t> opt_sdk_version;
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

  if (packet.has_android_soc_model()) {
    context_->metadata_tracker->SetMetadata(
        metadata::android_soc_model,
        Variadic::String(
            context_->storage->InternString(packet.android_soc_model())));
  }

  if (packet.has_android_hardware_revision()) {
    context_->metadata_tracker->SetMetadata(
        metadata::android_hardware_revision,
        Variadic::String(context_->storage->InternString(
            packet.android_hardware_revision())));
  }

  if (packet.has_android_storage_model()) {
    context_->metadata_tracker->SetMetadata(
        metadata::android_storage_model,
        Variadic::String(
            context_->storage->InternString(packet.android_storage_model())));
  }

  if (packet.has_android_ram_model()) {
    context_->metadata_tracker->SetMetadata(
        metadata::android_ram_model,
        Variadic::String(
            context_->storage->InternString(packet.android_ram_model())));
  }

  page_size_ = packet.page_size();
  if (!page_size_) {
    page_size_ = 4096;
  }

  if (packet.has_num_cpus()) {
    system_info_tracker->SetNumCpus(packet.num_cpus());
  }
}

void SystemProbesParser::ParseCpuInfo(ConstBytes blob) {
  protos::pbzero::CpuInfo::Decoder packet(blob.data, blob.size);
  std::vector<CpuInfo> cpu_infos;

  // Decode CpuInfo packet
  uint32_t cpu_id = 0;
  for (auto it = packet.cpus(); it; it++, cpu_id++) {
    protos::pbzero::CpuInfo::Cpu::Decoder cpu(*it);

    CpuInfo current_cpu_info;
    current_cpu_info.cpu = cpu_id;
    current_cpu_info.processor = cpu.processor();

    for (auto freq_it = cpu.frequencies(); freq_it; freq_it++) {
      uint32_t current_cpu_frequency = *freq_it;
      current_cpu_info.frequencies.push_back(current_cpu_frequency);
    }
    if (cpu.has_capacity()) {
      current_cpu_info.capacity = cpu.capacity();
    }

    if (cpu.has_arm_identifier()) {
      protos::pbzero::CpuInfo::ArmCpuIdentifier::Decoder identifier(
          cpu.arm_identifier());

      current_cpu_info.identifier = ArmCpuIdentifier{
          identifier.implementer(), identifier.architecture(),
          identifier.variant(),     identifier.part(),
          identifier.revision(),
      };
    }

    cpu_infos.push_back(current_cpu_info);
  }

  // Calculate cluster ids
  // We look to use capacities as it is an ARM provided metric which is designed
  // to measure the heterogeneity of CPU clusters however we fallback on the
  // maximum frequency as an estimate

  // Capacities are defined as existing on all CPUs if present and so we set
  // them as invalid if any is missing
  bool valid_capacities =
      std::all_of(cpu_infos.begin(), cpu_infos.end(),
                  [](CpuInfo info) { return info.capacity.has_value(); });

  bool valid_frequencies =
      std::all_of(cpu_infos.begin(), cpu_infos.end(),
                  [](CpuInfo info) { return !info.frequencies.empty(); });

  std::vector<uint32_t> cluster_ids(cpu_infos.size());
  uint32_t cluster_id = 0;

  if (valid_capacities) {
    std::sort(cpu_infos.begin(), cpu_infos.end(),
              [](auto a, auto b) { return a.capacity < b.capacity; });
    uint32_t previous_capacity = *cpu_infos[0].capacity;
    for (CpuInfo& cpu_info : cpu_infos) {
      uint32_t capacity = *cpu_info.capacity;
      // If cpus have the same capacity, they should have the same cluster id
      if (previous_capacity < capacity) {
        previous_capacity = capacity;
        cluster_id++;
      }
      cluster_ids[cpu_info.cpu] = cluster_id;
    }
  } else if (valid_frequencies) {
    // Use max frequency if capacities are invalid
    std::vector<CpuMaxFrequency> cpu_max_freqs;
    for (CpuInfo& info : cpu_infos) {
      cpu_max_freqs.push_back(
          {info.cpu, *std::max_element(info.frequencies.begin(),
                                       info.frequencies.end())});
    }
    std::sort(cpu_max_freqs.begin(), cpu_max_freqs.end(),
              [](auto a, auto b) { return a.max_frequency < b.max_frequency; });

    uint32_t previous_max_freq = cpu_max_freqs[0].max_frequency;
    for (CpuMaxFrequency& cpu_max_freq : cpu_max_freqs) {
      uint32_t max_freq = cpu_max_freq.max_frequency;
      // If cpus have the same max frequency, they should have the same
      // cluster_id
      if (previous_max_freq < max_freq) {
        previous_max_freq = max_freq;
        cluster_id++;
      }
      cluster_ids[cpu_max_freq.cpu] = cluster_id;
    }
  }

  // Add values to tables
  for (CpuInfo& cpu_info : cpu_infos) {
    tables::CpuTable::Id ucpu = context_->cpu_tracker->SetCpuInfo(
        cpu_info.cpu, cpu_info.processor, cluster_ids[cpu_info.cpu],
        cpu_info.capacity);
    for (uint32_t frequency : cpu_info.frequencies) {
      tables::CpuFreqTable::Row cpu_freq_row;
      cpu_freq_row.ucpu = ucpu;
      cpu_freq_row.freq = frequency;
      context_->storage->mutable_cpu_freq_table()->Insert(cpu_freq_row);
    }

    if (auto* id = std::get_if<ArmCpuIdentifier>(&cpu_info.identifier)) {
      context_->args_tracker->AddArgsTo(ucpu)
          .AddArg(arm_cpu_implementer,
                  Variadic::UnsignedInteger(id->implementer))
          .AddArg(arm_cpu_architecture,
                  Variadic::UnsignedInteger(id->architecture))
          .AddArg(arm_cpu_variant, Variadic::UnsignedInteger(id->variant))
          .AddArg(arm_cpu_part, Variadic::UnsignedInteger(id->part))
          .AddArg(arm_cpu_revision, Variadic::UnsignedInteger(id->revision));
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
