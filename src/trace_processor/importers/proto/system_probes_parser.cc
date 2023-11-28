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
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/system_info_tracker.h"
#include "src/trace_processor/importers/syscalls/syscall_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/types/trace_processor_context.h"

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
      cpu_freq_id_(context_->storage->InternString("cpufreq")) {
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
        TrackTracker::Group::kMemory, meminfo_strs_id_[key]);
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
    TrackId track = context_->track_tracker->InternGlobalCounterTrack(
        TrackTracker::Group::kClockFrequency, name);
    context_->event_tracker->PushCounter(ts, static_cast<double>(vm.value()),
                                         track);
  }

  uint32_t c = 0;
  for (auto it = sys_stats.cpufreq_khz(); it; ++it, ++c) {
    TrackId track =
        context_->track_tracker->InternCpuCounterTrack(cpu_freq_id_, c);
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
        cpu_times_user_ns_id_, ct.cpu_id());
    context_->event_tracker->PushCounter(ts, static_cast<double>(ct.user_ns()),
                                         track);

    track = context_->track_tracker->InternCpuCounterTrack(
        cpu_times_user_nice_ns_id_, ct.cpu_id());
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(ct.user_nice_ns()), track);

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

  for (auto it = sys_stats.buddy_info(); it; ++it) {
    protos::pbzero::SysStats::BuddyInfo::Decoder bi(*it);
    int order = 0;
    for (auto order_it = bi.order_pages(); order_it; ++order_it) {
      std::string node = bi.node().ToStdString();
      std::string zone = bi.zone().ToStdString();
      uint32_t size_kb =
          static_cast<uint32_t>(((1 << order) * page_size_) / 1024);
      base::StackString<255> counter_name("mem.buddyinfo[%s][%s][%u kB]",
                                          node.c_str(), zone.c_str(), size_kb);
      StringId name =
          context_->storage->InternString(counter_name.string_view());
      TrackId track = context_->track_tracker->InternGlobalCounterTrack(
          TrackTracker::Group::kMemory, name);
      context_->event_tracker->PushCounter(ts, static_cast<double>(*order_it),
                                           track);
      order++;
    }
  }

  for (auto it = sys_stats.disk_stat(); it; ++it) {
    ParseDiskStats(ts, *it);
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
      if (fld.id() == protos::pbzero::ProcessStats::Process::kFdsFieldNumber) {
        ParseProcessFds(ts, pid, fld.as_bytes());
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
  if (packet.has_utsname()) {
    ConstBytes utsname_blob = packet.utsname();
    protos::pbzero::Utsname::Decoder utsname(utsname_blob.data,
                                             utsname_blob.size);
    base::StringView machine = utsname.machine();
    SyscallTracker* syscall_tracker = SyscallTracker::GetOrCreate(context_);
    Architecture arch = SyscallTable::ArchFromString(machine);
    if (arch != kUnknown) {
      syscall_tracker->SetArchitecture(arch);
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

  if (packet.has_timezone_off_mins()) {
    context_->metadata_tracker->SetMetadata(
        metadata::timezone_off_mins,
        Variadic::Integer(packet.timezone_off_mins()));
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

  int64_t hz = packet.hz();
  if (hz > 0)
    ms_per_tick_ = 1000u / static_cast<uint64_t>(hz);

  page_size_ = packet.page_size();
  if (!page_size_)
    page_size_ = 4096;
}

void SystemProbesParser::ParseCpuInfo(ConstBytes blob) {
  protos::pbzero::CpuInfo::Decoder packet(blob.data, blob.size);
  uint32_t cluster_id = 0;
  std::vector<uint32_t> last_cpu_freqs;
  for (auto it = packet.cpus(); it; it++) {
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
    if (freqs != last_cpu_freqs && !last_cpu_freqs.empty()) {
      cluster_id++;
    }
    cpu_row.cluster_id = cluster_id;

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
