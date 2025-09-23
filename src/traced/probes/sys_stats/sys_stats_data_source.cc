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

#include "src/traced/probes/sys_stats/sys_stats_data_source.h"

#include <stdlib.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <limits>
#include <utility>

#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/metatrace.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/traced/sys_stats_counters.h"

#include "protos/perfetto/common/sys_stats_counters.pbzero.h"
#include "protos/perfetto/config/sys_stats/sys_stats_config.pbzero.h"
#include "protos/perfetto/trace/sys_stats/sys_stats.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

using protos::pbzero::SysStatsConfig;

namespace {
constexpr size_t kReadBufSize = 1024 * 16;

base::ScopedFile OpenReadOnly(const char* path) {
  base::ScopedFile fd(base::OpenFile(path, O_RDONLY));
  if (!fd)
    PERFETTO_PLOG("Failed opening %s", path);
  return fd;
}

uint32_t ClampTo10Ms(uint32_t period_ms, const char* counter_name) {
  if (period_ms > 0 && period_ms < 10) {
    PERFETTO_ILOG("%s %" PRIu32
                  " is less than minimum of 10ms. Increasing to 10ms.",
                  counter_name, period_ms);
    return 10;
  }
  return period_ms;
}

}  // namespace

// static
const ProbesDataSource::Descriptor SysStatsDataSource::descriptor = {
    /*name*/ "linux.sys_stats",
    /*flags*/ Descriptor::kFlagsNone,
    /*fill_descriptor_func*/ nullptr,
};

SysStatsDataSource::SysStatsDataSource(
    base::TaskRunner* task_runner,
    TracingSessionID session_id,
    std::unique_ptr<TraceWriter> writer,
    const DataSourceConfig& ds_config,
    std::unique_ptr<CpuFreqInfo> cpu_freq_info,
    OpenFunction open_fn)
    : ProbesDataSource(session_id, &descriptor),
      task_runner_(task_runner),
      writer_(std::move(writer)),
      cpu_freq_info_(std::move(cpu_freq_info)),
      open_fn_(open_fn ? open_fn : OpenReadOnly),
      weak_factory_(this) {
  ns_per_user_hz_ = 1000000000ull / static_cast<uint64_t>(sysconf(_SC_CLK_TCK));
  meminfo_fd_ = open_fn_("/proc/meminfo");
  vmstat_fd_ = open_fn_("/proc/vmstat");
  stat_fd_ = open_fn_("/proc/stat");
  buddy_fd_ = open_fn_("/proc/buddyinfo");
  diskstat_fd_ = open_fn_("/proc/diskstats");
  psi_cpu_fd_ = open_fn_("/proc/pressure/cpu");
  psi_io_fd_ = open_fn_("/proc/pressure/io");
  psi_memory_fd_ = open_fn_("/proc/pressure/memory");
  read_buf_ = base::PagedMemory::Allocate(kReadBufSize);

  // Build a lookup map that allows to quickly translate strings like "MemTotal"
  // into the corresponding enum value, only for the counters enabled in the
  // config.

  using protos::pbzero::SysStatsConfig;
  SysStatsConfig::Decoder cfg(ds_config.sys_stats_config_raw());

  constexpr size_t kMaxMeminfoEnum = protos::pbzero::MeminfoCounters_MAX;
  std::bitset<kMaxMeminfoEnum + 1> meminfo_counters_enabled{};
  if (!cfg.has_meminfo_counters())
    meminfo_counters_enabled.set();
  for (auto it = cfg.meminfo_counters(); it; ++it) {
    uint32_t counter = static_cast<uint32_t>(*it);
    if (counter > 0 && counter <= kMaxMeminfoEnum) {
      meminfo_counters_enabled.set(counter);
    } else {
      PERFETTO_DFATAL("Meminfo counter out of bounds %u", counter);
    }
  }
  for (size_t i = 0; i < base::ArraySize(kMeminfoKeys); i++) {
    const auto& k = kMeminfoKeys[i];
    if (meminfo_counters_enabled[static_cast<size_t>(k.id)])
      meminfo_counters_.emplace(k.str, k.id);
  }

  constexpr size_t kMaxVmstatEnum = protos::pbzero::VmstatCounters_MAX;
  std::bitset<kMaxVmstatEnum + 1> vmstat_counters_enabled{};
  if (!cfg.has_vmstat_counters())
    vmstat_counters_enabled.set();
  for (auto it = cfg.vmstat_counters(); it; ++it) {
    uint32_t counter = static_cast<uint32_t>(*it);
    if (counter > 0 && counter <= kMaxVmstatEnum) {
      vmstat_counters_enabled.set(counter);
    } else {
      PERFETTO_DFATAL("Vmstat counter out of bounds %u", counter);
    }
  }
  for (size_t i = 0; i < base::ArraySize(kVmstatKeys); i++) {
    const auto& k = kVmstatKeys[i];
    if (vmstat_counters_enabled[static_cast<size_t>(k.id)])
      vmstat_counters_.emplace(k.str, k.id);
  }

  if (!cfg.has_stat_counters())
    stat_enabled_fields_ = ~0u;
  for (auto counter = cfg.stat_counters(); counter; ++counter) {
    stat_enabled_fields_ |= 1ul << static_cast<uint32_t>(*counter);
  }

  std::array<uint32_t, 12> periods_ms{};
  std::array<uint32_t, 12> ticks{};
  static_assert(periods_ms.size() == ticks.size(), "must have same size");

  periods_ms[0] = ClampTo10Ms(cfg.meminfo_period_ms(), "meminfo_period_ms");
  periods_ms[1] = ClampTo10Ms(cfg.vmstat_period_ms(), "vmstat_period_ms");
  periods_ms[2] = ClampTo10Ms(cfg.stat_period_ms(), "stat_period_ms");
  periods_ms[3] = ClampTo10Ms(cfg.devfreq_period_ms(), "devfreq_period_ms");
  periods_ms[4] = ClampTo10Ms(cfg.cpufreq_period_ms(), "cpufreq_period_ms");
  periods_ms[5] = ClampTo10Ms(cfg.buddyinfo_period_ms(), "buddyinfo_period_ms");
  periods_ms[6] = ClampTo10Ms(cfg.diskstat_period_ms(), "diskstat_period_ms");
  periods_ms[7] = ClampTo10Ms(cfg.psi_period_ms(), "psi_period_ms");
  periods_ms[8] = ClampTo10Ms(cfg.thermal_period_ms(), "thermal_period_ms");
  periods_ms[9] = ClampTo10Ms(cfg.cpuidle_period_ms(), "cpuidle_period_ms");
  periods_ms[10] = ClampTo10Ms(cfg.gpufreq_period_ms(), "gpufreq_period_ms");
  periods_ms[11] = ClampTo10Ms(cfg.cgroup_period_ms(), "cgroup_period_ms");

  tick_period_ms_ = 0;
  for (uint32_t ms : periods_ms) {
    if (ms && (ms < tick_period_ms_ || tick_period_ms_ == 0))
      tick_period_ms_ = ms;
  }

  if (tick_period_ms_ == 0)
    return;  // No polling configured.

  for (size_t i = 0; i < periods_ms.size(); i++) {
    auto ms = periods_ms[i];
    if (ms && ms % tick_period_ms_ != 0) {
      PERFETTO_ELOG("SysStat periods are not integer multiples of each other");
      return;
    }
    ticks[i] = ms / tick_period_ms_;
  }
  meminfo_ticks_ = ticks[0];
  vmstat_ticks_ = ticks[1];
  stat_ticks_ = ticks[2];
  devfreq_ticks_ = ticks[3];
  cpufreq_ticks_ = ticks[4];
  buddyinfo_ticks_ = ticks[5];
  diskstat_ticks_ = ticks[6];
  psi_ticks_ = ticks[7];
  thermal_ticks_ = ticks[8];
  cpuidle_ticks_ = ticks[9];
  gpufreq_ticks_ = ticks[10];
  cgroup_ticks_ = ticks[11];

  // Build cgroup counters lookup map for enabled counters
  constexpr size_t kMaxCgroupEnum = protos::pbzero::CgroupCounters_MAX;
  std::bitset<kMaxCgroupEnum + 1> cgroup_counters_enabled{};
  if (!cfg.has_cgroup_counters())
    cgroup_counters_enabled.set();
  for (auto it = cfg.cgroup_counters(); it; ++it) {
    uint32_t counter = static_cast<uint32_t>(*it);
    if (counter > 0 && counter <= kMaxCgroupEnum) {
      cgroup_counters_enabled.set(counter);
    } else {
      PERFETTO_DFATAL("Cgroup counter out of bounds %u", counter);
    }
  }
  for (size_t i = 0; i < base::ArraySize(kCgroupKeys); i++) {
    const auto& k = kCgroupKeys[i];
    if (cgroup_counters_enabled[static_cast<size_t>(k.id)])
      cgroup_counters_.emplace(k.str, k.id);
  }

  // Configure cgroup paths to monitor
  for (auto it = cfg.cgroup_paths(); it; ++it) {
    cgroup_paths_.emplace_back(reinterpret_cast<const char*>(it->data()),
                               it->size());
  }

  // Use default Android cgroup paths if none specified
  if (cgroup_paths_.empty()) {
    cgroup_paths_ = {
        "/sys/fs/cgroup/cpu/top-app",
        "/sys/fs/cgroup/cpu/foreground",
        "/sys/fs/cgroup/cpu/background",
        "/sys/fs/cgroup/cpu/system-background",
        "/sys/fs/cgroup/memory/top-app",
        "/sys/fs/cgroup/memory/foreground",
        "/sys/fs/cgroup/memory/background",
        "/sys/fs/cgroup/memory/system-background",
    };
  }
}

void SysStatsDataSource::Start() {
  auto weak_this = GetWeakPtr();
  task_runner_->PostTask(std::bind(&SysStatsDataSource::Tick, weak_this));
}

// static
void SysStatsDataSource::Tick(base::WeakPtr<SysStatsDataSource> weak_this) {
  if (!weak_this)
    return;
  SysStatsDataSource& thiz = *weak_this;

  uint32_t period_ms = thiz.tick_period_ms_;
  uint32_t delay_ms =
      period_ms -
      static_cast<uint32_t>(base::GetWallTimeMs().count() % period_ms);
  thiz.task_runner_->PostDelayedTask(
      std::bind(&SysStatsDataSource::Tick, weak_this), delay_ms);
  thiz.ReadSysStats();
}

SysStatsDataSource::~SysStatsDataSource() = default;

void SysStatsDataSource::ReadSysStats() {
  PERFETTO_METATRACE_SCOPED(TAG_PROC_POLLERS, READ_SYS_STATS);
  auto packet = writer_->NewTracePacket();

  packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));
  auto* sys_stats = packet->set_sys_stats();

  if (meminfo_ticks_ && tick_ % meminfo_ticks_ == 0)
    ReadMeminfo(sys_stats);

  if (vmstat_ticks_ && tick_ % vmstat_ticks_ == 0)
    ReadVmstat(sys_stats);

  if (stat_ticks_ && tick_ % stat_ticks_ == 0)
    ReadStat(sys_stats);

  if (devfreq_ticks_ && tick_ % devfreq_ticks_ == 0)
    ReadDevfreq(sys_stats);

  if (cpufreq_ticks_ && tick_ % cpufreq_ticks_ == 0)
    ReadCpufreq(sys_stats);

  if (buddyinfo_ticks_ && tick_ % buddyinfo_ticks_ == 0)
    ReadBuddyInfo(sys_stats);

  if (diskstat_ticks_ && tick_ % diskstat_ticks_ == 0)
    ReadDiskStat(sys_stats);

  if (psi_ticks_ && tick_ % psi_ticks_ == 0)
    ReadPsi(sys_stats);

  if (thermal_ticks_ && tick_ % thermal_ticks_ == 0)
    ReadThermalZones(sys_stats);

  if (cpuidle_ticks_ && tick_ % cpuidle_ticks_ == 0)
    ReadCpuIdleStates(sys_stats);

  if (gpufreq_ticks_ && tick_ % gpufreq_ticks_ == 0)
    ReadGpuFrequency(sys_stats);

  if (cgroup_ticks_ && tick_ % cgroup_ticks_ == 0)
    ReadCgroup(sys_stats);

  sys_stats->set_collection_end_timestamp(
      static_cast<uint64_t>(base::GetBootTimeNs().count()));

  tick_++;
}

base::ScopedDir SysStatsDataSource::OpenDirAndLogOnErrorOnce(
    const std::string& dir_path,
    bool* already_logged) {
  base::ScopedDir dir(opendir(dir_path.c_str()));
  if (!dir && !(*already_logged)) {
    PERFETTO_PLOG("Failed to open %s", dir_path.c_str());
    *already_logged = true;
  }
  return dir;
}

std::optional<std::string> SysStatsDataSource::ReadFileToString(
    const std::string& path) {
  base::ScopedFile fd = OpenReadOnly(path.c_str());
  if (!fd) {
    return std::nullopt;
  }
  size_t rsize = ReadFile(&fd, path.c_str());
  if (!rsize)
    return std::nullopt;
  return base::StripSuffix(static_cast<char*>(read_buf_.Get()), "\n");
}

std::optional<uint64_t> SysStatsDataSource::ReadFileToUInt64(
    const std::string& path) {
  base::ScopedFile fd = OpenReadOnly(path.c_str());
  if (!fd) {
    return std::nullopt;
  }
  size_t rsize = ReadFile(&fd, path.c_str());
  if (!rsize)
    return std::nullopt;

  return static_cast<uint64_t>(
      strtoll(static_cast<char*>(read_buf_.Get()), nullptr, 10));
}

void SysStatsDataSource::ReadThermalZones(protos::pbzero::SysStats* sys_stats) {
  std::string base_dir = "/sys/class/thermal/";
  base::ScopedDir thermal_dir =
      OpenDirAndLogOnErrorOnce(base_dir, &thermal_error_logged_);
  if (!thermal_dir) {
    return;
  }
  while (struct dirent* dir_ent = readdir(*thermal_dir)) {
    // Entries in /sys/class/thermal are symlinks to /devices/virtual
    if (dir_ent->d_type != DT_LNK)
      continue;
    const char* name = dir_ent->d_name;
    if (!base::StartsWith(name, "thermal_zone")) {
      continue;
    }
    auto* thermal_zone = sys_stats->add_thermal_zone();
    thermal_zone->set_name(name);
    base::StackString<256> thermal_zone_temp_path("/sys/class/thermal/%s/temp",
                                                  name);
    auto temp = ReadFileToUInt64(thermal_zone_temp_path.ToStdString());
    if (temp) {
      thermal_zone->set_temp(*temp / 1000);
    }
    base::StackString<256> thermal_zone_type_path("/sys/class/thermal/%s/type",
                                                  name);
    auto type = ReadFileToString(thermal_zone_type_path.ToStdString());
    if (type) {
      thermal_zone->set_type(*type);
    }
  }
}

void SysStatsDataSource::ReadCpuIdleStates(
    protos::pbzero::SysStats* sys_stats) {
  std::string cpu_dir_path = "/sys/devices/system/cpu/";
  base::ScopedDir cpu_dir =
      OpenDirAndLogOnErrorOnce(cpu_dir_path, &cpuidle_error_logged_);
  if (!cpu_dir) {
    return;
  }
  // Iterate over all CPUs.
  while (struct dirent* cpu_dir_ent = readdir(*cpu_dir)) {
    const char* cpu_name = cpu_dir_ent->d_name;
    if (!base::StartsWith(cpu_name, "cpu"))
      continue;
    auto maybe_cpu_index =
        base::StringToUInt32(base::StripPrefix(cpu_name, "cpu"));
    if (!maybe_cpu_index.has_value()) {
      continue;
    }
    uint32_t cpu_id = maybe_cpu_index.value();

    auto* cpuidle_stats = sys_stats->add_cpuidle_state();
    cpuidle_stats->set_cpu_id(cpu_id);
    std::string cpuidle_path =
        "/sys/devices/system/cpu/" + std::string(cpu_name) + "/cpuidle/";
    base::ScopedDir cpu_state_dir =
        OpenDirAndLogOnErrorOnce(cpuidle_path, &cpuidle_error_logged_);
    if (!cpu_state_dir) {
      return;
    }
    // Iterate over all CPU idle states.
    while (struct dirent* state_dir_ent = readdir(*cpu_state_dir)) {
      const char* state_name = state_dir_ent->d_name;
      if (!base::StartsWith(state_name, "state"))
        continue;
      base::StackString<256> cpuidle_state_name_path(
          "/sys/devices/system/cpu/%s/cpuidle/%s/name", cpu_name, state_name);
      auto cpuidle_state_name =
          ReadFileToString(cpuidle_state_name_path.ToStdString());

      base::StackString<256> cpuidle_state_time_path(
          "/sys/devices/system/cpu/%s/cpuidle/%s/time", cpu_name, state_name);
      auto time = ReadFileToUInt64(cpuidle_state_time_path.ToStdString());
      if (!cpuidle_state_name || !time) {
        continue;
      }
      auto cpuidle_state = cpuidle_stats->add_cpuidle_state_entry();
      cpuidle_state->set_state(*cpuidle_state_name);
      cpuidle_state->set_duration_us(*time);
    }
  }
}

std::optional<uint64_t> SysStatsDataSource::ReadAMDGpuFreq() {
  std::optional<std::string> amd_gpu_freq =
      ReadFileToString("/sys/class/drm/card0/device/pp_dpm_sclk");
  if (!amd_gpu_freq) {
    return std::nullopt;
  }
  for (base::StringSplitter lines(*amd_gpu_freq, '\n'); lines.Next();) {
    base::StringView line(lines.cur_token(), lines.cur_token_size());
    // Current frequency indicated with asterisk.
    if (line.EndsWith("*")) {
      for (base::StringSplitter words(line.ToStdString(), ' '); words.Next();) {
        if (!base::EndsWith(words.cur_token(), "Mhz"))
          continue;
        // Strip suffix "Mhz".
        std::string maybe_freq = std::string(words.cur_token())
                                     .substr(0, words.cur_token_size() - 3);
        auto freq = base::StringToUInt32(maybe_freq);
        return freq;
      }
    }
  }
  return std::nullopt;
}

void SysStatsDataSource::ReadGpuFrequency(protos::pbzero::SysStats* sys_stats) {
  // For Adreno GPUs.
  auto freq = ReadFileToUInt64("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq");
  if (freq) {
    sys_stats->add_gpufreq_mhz((*freq) / 1'000'000);
    return;
  }

  // For Intel GPUs.
  freq = ReadFileToUInt64("/sys/class/drm/card0/gt_act_freq_mhz");
  if (freq) {
    sys_stats->add_gpufreq_mhz(*freq);
    return;
  }

  // For AMD GPUs.
  freq = ReadAMDGpuFreq();
  if (freq) {
    sys_stats->add_gpufreq_mhz(*freq);
  }
}

void SysStatsDataSource::ReadDiskStat(protos::pbzero::SysStats* sys_stats) {
  size_t rsize = ReadFile(&diskstat_fd_, "/proc/diskstats");
  if (!rsize) {
    return;
  }

  char* buf = static_cast<char*>(read_buf_.Get());
  for (base::StringSplitter lines(buf, rsize, '\n'); lines.Next();) {
    uint32_t index = 0;
    auto* disk_stat = sys_stats->add_disk_stat();
    for (base::StringSplitter words(&lines, ' '); words.Next(); index++) {
      if (index == 2) {  // index for device name (string)
        disk_stat->set_device_name(words.cur_token());
      } else if (index >= 5) {  // integer values from index 5
        std::optional<uint64_t> value_address =
            base::CStringToUInt64(words.cur_token());
        uint64_t value = value_address ? *value_address : 0;

        switch (index) {
          case 5:
            disk_stat->set_read_sectors(value);
            break;
          case 6:
            disk_stat->set_read_time_ms(value);
            break;
          case 9:
            disk_stat->set_write_sectors(value);
            break;
          case 10:
            disk_stat->set_write_time_ms(value);
            break;
          case 16:
            disk_stat->set_discard_sectors(value);
            break;
          case 17:
            disk_stat->set_discard_time_ms(value);
            break;
          case 18:
            disk_stat->set_flush_count(value);
            break;
          case 19:
            disk_stat->set_flush_time_ms(value);
            break;
        }

        if (index == 19) {
          break;
        }
      }
    }
  }
}

void SysStatsDataSource::ReadPsi(protos::pbzero::SysStats* sys_stats) {
  using PsiSample = protos::pbzero::SysStats::PsiSample;

  auto read_psi_resource = [this, sys_stats](
                               base::ScopedFile* file, const char* path,
                               PsiSample::PsiResource resource_some,
                               PsiSample::PsiResource resource_full) {
    size_t rsize = ReadFile(file, path);
    if (!rsize) {
      return;
    }

    char* buf = static_cast<char*>(read_buf_.Get());
    for (base::StringSplitter lines(buf, rsize, '\n'); lines.Next();) {
      uint32_t index = 0;
      auto* psi = sys_stats->add_psi();
      for (base::StringSplitter words(&lines, ' '); words.Next(); ++index) {
        base::StringView token(words.cur_token(), words.cur_token_size());
        // A single line is of the form (note we skip avg parsing, indexes 1-3):
        //     some avg10=0.00 avg60=0.00 avg300=0.00 total=0
        if (index == 0) {
          auto resource = token == "some" ? resource_some
                          : token == "full"
                              ? resource_full
                              : PsiSample::PSI_RESOURCE_UNSPECIFIED;
          psi->set_resource(resource);
        } else if (index == 4) {
          const base::StringView prefix("total=");
          if (token.StartsWith(prefix)) {
            token = token.substr(prefix.size());
          }
          // The raw PSI total readings are in micros, so convert accordingly.
          std::optional<uint64_t> total_us =
              base::CStringToUInt64(token.data());
          uint64_t total_ns = total_us ? *total_us * 1000 : 0;
          psi->set_total_ns(total_ns);
        } else if (index > 4) {
          break;
        }
      }
    }
  };

  read_psi_resource(&psi_cpu_fd_, "/proc/pressure/cpu",
                    PsiSample::PSI_RESOURCE_CPU_SOME,
                    PsiSample::PSI_RESOURCE_CPU_FULL);
  read_psi_resource(&psi_io_fd_, "/proc/pressure/io",
                    PsiSample::PSI_RESOURCE_IO_SOME,
                    PsiSample::PSI_RESOURCE_IO_FULL);
  read_psi_resource(&psi_memory_fd_, "/proc/pressure/memory",
                    PsiSample::PSI_RESOURCE_MEMORY_SOME,
                    PsiSample::PSI_RESOURCE_MEMORY_FULL);
}

void SysStatsDataSource::ReadBuddyInfo(protos::pbzero::SysStats* sys_stats) {
  size_t rsize = ReadFile(&buddy_fd_, "/proc/buddyinfo");
  if (!rsize) {
    return;
  }

  char* buf = static_cast<char*>(read_buf_.Get());
  for (base::StringSplitter lines(buf, rsize, '\n'); lines.Next();) {
    uint32_t index = 0;
    auto* buddy_info = sys_stats->add_buddy_info();
    for (base::StringSplitter words(&lines, ' '); words.Next();) {
      if (index == 1) {
        std::string token = words.cur_token();
        token = token.substr(0, token.find(","));
        buddy_info->set_node(token);
      } else if (index == 3) {
        buddy_info->set_zone(words.cur_token());
      } else if (index > 3) {
        buddy_info->add_order_pages(
            static_cast<uint32_t>(strtoul(words.cur_token(), nullptr, 0)));
      }
      index++;
    }
  }
}

void SysStatsDataSource::ReadDevfreq(protos::pbzero::SysStats* sys_stats) {
  std::string base_dir = "/sys/class/devfreq/";
  base::ScopedDir devfreq_dir =
      OpenDirAndLogOnErrorOnce(base_dir, &devfreq_error_logged_);
  if (!devfreq_dir) {
    return;
  }
  while (struct dirent* dir_ent = readdir(*devfreq_dir)) {
    // Entries in /sys/class/devfreq are symlinks to /devices/platform
    if (dir_ent->d_type != DT_LNK)
      continue;
    const char* name = dir_ent->d_name;
    const char* file_content = ReadDevfreqCurFreq(name);
    auto value = static_cast<uint64_t>(strtoll(file_content, nullptr, 10));
    auto* devfreq = sys_stats->add_devfreq();
    devfreq->set_key(name);
    devfreq->set_value(value);
  }
}

void SysStatsDataSource::ReadCpufreq(protos::pbzero::SysStats* sys_stats) {
  const auto& cpufreq = cpu_freq_info_->ReadCpuCurrFreq();

  for (const auto& c : cpufreq)
    sys_stats->add_cpufreq_khz(c);
}

const char* SysStatsDataSource::ReadDevfreqCurFreq(
    const std::string& deviceName) {
  const char* devfreq_base_path = "/sys/class/devfreq";
  const char* freq_file_name = "cur_freq";
  base::StackString<256> cur_freq_path("%s/%s/%s", devfreq_base_path,
                                       deviceName.c_str(), freq_file_name);
  base::ScopedFile fd = OpenReadOnly(cur_freq_path.c_str());
  if (!fd && !devfreq_error_logged_) {
    devfreq_error_logged_ = true;
    PERFETTO_PLOG("Failed to open %s", cur_freq_path.c_str());
    return "";
  }
  size_t rsize = ReadFile(&fd, cur_freq_path.c_str());
  if (!rsize)
    return "";
  return static_cast<char*>(read_buf_.Get());
}

void SysStatsDataSource::ReadMeminfo(protos::pbzero::SysStats* sys_stats) {
  size_t rsize = ReadFile(&meminfo_fd_, "/proc/meminfo");
  if (!rsize)
    return;
  char* buf = static_cast<char*>(read_buf_.Get());
  for (base::StringSplitter lines(buf, rsize, '\n'); lines.Next();) {
    base::StringSplitter words(&lines, ' ');
    if (!words.Next())
      continue;
    // Extract the meminfo key, dropping trailing ':' (e.g., "MemTotal: NN KB").
    words.cur_token()[words.cur_token_size() - 1] = '\0';
    auto it = meminfo_counters_.find(words.cur_token());
    if (it == meminfo_counters_.end())
      continue;
    int counter_id = it->second;
    if (!words.Next())
      continue;
    auto value = static_cast<uint64_t>(strtoll(words.cur_token(), nullptr, 10));
    auto* meminfo = sys_stats->add_meminfo();
    meminfo->set_key(static_cast<protos::pbzero::MeminfoCounters>(counter_id));
    meminfo->set_value(value);
  }
}

void SysStatsDataSource::ReadVmstat(protos::pbzero::SysStats* sys_stats) {
  size_t rsize = ReadFile(&vmstat_fd_, "/proc/vmstat");
  if (!rsize)
    return;
  char* buf = static_cast<char*>(read_buf_.Get());
  for (base::StringSplitter lines(buf, rsize, '\n'); lines.Next();) {
    base::StringSplitter words(&lines, ' ');
    if (!words.Next())
      continue;
    auto it = vmstat_counters_.find(words.cur_token());
    if (it == vmstat_counters_.end())
      continue;
    int counter_id = it->second;
    if (!words.Next())
      continue;
    auto value = static_cast<uint64_t>(strtoll(words.cur_token(), nullptr, 10));
    auto* vmstat = sys_stats->add_vmstat();
    vmstat->set_key(static_cast<protos::pbzero::VmstatCounters>(counter_id));
    vmstat->set_value(value);
  }
}

void SysStatsDataSource::ReadStat(protos::pbzero::SysStats* sys_stats) {
  size_t rsize = ReadFile(&stat_fd_, "/proc/stat");
  if (!rsize)
    return;
  char* buf = static_cast<char*>(read_buf_.Get());
  for (base::StringSplitter lines(buf, rsize, '\n'); lines.Next();) {
    base::StringSplitter words(&lines, ' ');
    if (!words.Next())
      continue;

    // Per-CPU stats.
    if ((stat_enabled_fields_ & (1 << SysStatsConfig::STAT_CPU_TIMES)) &&
        words.cur_token_size() > 3 && !strncmp(words.cur_token(), "cpu", 3)) {
      long cpu_id = strtol(words.cur_token() + 3, nullptr, 10);
      std::array<uint64_t, 8> cpu_times{};
      for (size_t i = 0; i < cpu_times.size() && words.Next(); i++) {
        cpu_times[i] =
            static_cast<uint64_t>(strtoll(words.cur_token(), nullptr, 10));
      }
      auto* cpu_stat = sys_stats->add_cpu_stat();
      cpu_stat->set_cpu_id(static_cast<uint32_t>(cpu_id));
      cpu_stat->set_user_ns(cpu_times[0] * ns_per_user_hz_);
      cpu_stat->set_user_nice_ns(cpu_times[1] * ns_per_user_hz_);
      cpu_stat->set_system_mode_ns(cpu_times[2] * ns_per_user_hz_);
      cpu_stat->set_idle_ns(cpu_times[3] * ns_per_user_hz_);
      cpu_stat->set_io_wait_ns(cpu_times[4] * ns_per_user_hz_);
      cpu_stat->set_irq_ns(cpu_times[5] * ns_per_user_hz_);
      cpu_stat->set_softirq_ns(cpu_times[6] * ns_per_user_hz_);
      cpu_stat->set_steal_ns(cpu_times[7] * ns_per_user_hz_);
    }
    // IRQ counters
    else if ((stat_enabled_fields_ & (1 << SysStatsConfig::STAT_IRQ_COUNTS)) &&
             !strcmp(words.cur_token(), "intr")) {
      for (size_t i = 0; words.Next(); i++) {
        auto v = static_cast<uint64_t>(strtoll(words.cur_token(), nullptr, 10));
        if (i == 0) {
          sys_stats->set_num_irq_total(v);
        } else if (v > 0) {
          auto* irq_stat = sys_stats->add_num_irq();
          irq_stat->set_irq(static_cast<int32_t>(i - 1));
          irq_stat->set_count(v);
        }
      }
    }
    // Softirq counters.
    else if ((stat_enabled_fields_ &
              (1 << SysStatsConfig::STAT_SOFTIRQ_COUNTS)) &&
             !strcmp(words.cur_token(), "softirq")) {
      for (size_t i = 0; words.Next(); i++) {
        auto v = static_cast<uint64_t>(strtoll(words.cur_token(), nullptr, 10));
        if (i == 0) {
          sys_stats->set_num_softirq_total(v);
        } else {
          auto* softirq_stat = sys_stats->add_num_softirq();
          softirq_stat->set_irq(static_cast<int32_t>(i - 1));
          softirq_stat->set_count(v);
        }
      }
    }
    // Number of forked processes since boot.
    else if ((stat_enabled_fields_ & (1 << SysStatsConfig::STAT_FORK_COUNT)) &&
             !strcmp(words.cur_token(), "processes")) {
      if (words.Next()) {
        sys_stats->set_num_forks(
            static_cast<uint64_t>(strtoll(words.cur_token(), nullptr, 10)));
      }
    }

  }  // for (line)
}

base::WeakPtr<SysStatsDataSource> SysStatsDataSource::GetWeakPtr() const {
  return weak_factory_.GetWeakPtr();
}

void SysStatsDataSource::Flush(FlushRequestID, std::function<void()> callback) {
  writer_->Flush(callback);
}

void SysStatsDataSource::ReadCgroup(protos::pbzero::SysStats* sys_stats) {
  for (const auto& cgroup_path : cgroup_paths_) {
    // Determine which files to read based on cgroup type
    if (cgroup_path.find("/cpu/") != std::string::npos ||
        cgroup_path.find("/cpu") == cgroup_path.length() - 4) {
      ReadCgroupFile(sys_stats, cgroup_path, "cpu.stat", true);
    }

    if (cgroup_path.find("/memory/") != std::string::npos ||
        cgroup_path.find("/memory") == cgroup_path.length() - 7) {
      ReadCgroupFile(sys_stats, cgroup_path, "memory.stat", true);
      ReadCgroupFile(sys_stats, cgroup_path, "memory.current", false);
      ReadCgroupFile(sys_stats, cgroup_path, "memory.max", false);
      ReadCgroupFile(sys_stats, cgroup_path, "memory.swap.current", false);
      ReadCgroupFile(sys_stats, cgroup_path, "memory.swap.max", false);
    }

    // IO stats can be in various cgroup types
    ReadCgroupFile(sys_stats, cgroup_path, "io.stat", false);
  }
}

void SysStatsDataSource::ReadCgroupFile(protos::pbzero::SysStats* sys_stats,
                                        const std::string& cgroup_path,
                                        const std::string& file_name,
                                        bool log_errors) {
  std::string full_path = cgroup_path + "/" + file_name;
  base::ScopedFile fd = open_fn_(full_path.c_str());
  if (!fd) {
    if (log_errors && !cgroup_error_logged_) {
      PERFETTO_PLOG("Failed to open %s", full_path.c_str());
      cgroup_error_logged_ = true;
    }
    return;
  }

  size_t rsize = ReadFile(&fd, full_path.c_str());
  if (!rsize)
    return;

  char* buf = static_cast<char*>(read_buf_.Get());

  if (file_name == "cpu.stat") {
    ParseCgroupCpuStat(sys_stats, buf, rsize, cgroup_path);
  } else if (file_name == "memory.stat") {
    ParseCgroupMemoryStat(sys_stats, buf, rsize, cgroup_path);
  } else if (file_name == "memory.current" || file_name == "memory.max" ||
             file_name == "memory.swap.current" ||
             file_name == "memory.swap.max") {
    ParseCgroupSingleValue(sys_stats, buf, file_name, cgroup_path);
  } else if (file_name == "io.stat") {
    ParseCgroupIoStat(sys_stats, buf, rsize, cgroup_path);
  }
}

void SysStatsDataSource::ParseCgroupCpuStat(protos::pbzero::SysStats* sys_stats,
                                            char* buf,
                                            size_t rsize,
                                            const std::string& cgroup_path) {
  for (base::StringSplitter lines(buf, rsize, '\n'); lines.Next();) {
    base::StringSplitter words(&lines, ' ');
    if (!words.Next())
      continue;

    auto it = cgroup_counters_.find(words.cur_token());
    if (it == cgroup_counters_.end())
      continue;

    int counter_id = it->second;
    if (!words.Next())
      continue;

    auto value = static_cast<uint64_t>(strtoll(words.cur_token(), nullptr, 10));
    auto* cgroup_value = sys_stats->add_cgroup();
    cgroup_value->set_key(
        static_cast<protos::pbzero::CgroupCounters>(counter_id));
    cgroup_value->set_value(value);
    cgroup_value->set_cgroup_path(cgroup_path);
  }
}

void SysStatsDataSource::ParseCgroupMemoryStat(
    protos::pbzero::SysStats* sys_stats,
    char* buf,
    size_t rsize,
    const std::string& cgroup_path) {
  for (base::StringSplitter lines(buf, rsize, '\n'); lines.Next();) {
    base::StringSplitter words(&lines, ' ');
    if (!words.Next())
      continue;

    auto it = cgroup_counters_.find(words.cur_token());
    if (it == cgroup_counters_.end())
      continue;

    int counter_id = it->second;
    if (!words.Next())
      continue;

    auto value = static_cast<uint64_t>(strtoll(words.cur_token(), nullptr, 10));
    auto* cgroup_value = sys_stats->add_cgroup();
    cgroup_value->set_key(
        static_cast<protos::pbzero::CgroupCounters>(counter_id));
    cgroup_value->set_value(value);
    cgroup_value->set_cgroup_path(cgroup_path);
  }
}

void SysStatsDataSource::ParseCgroupSingleValue(
    protos::pbzero::SysStats* sys_stats,
    char* buf,
    const std::string& file_name,
    const std::string& cgroup_path) {
  // Map filename to counter enum
  protos::pbzero::CgroupCounters counter_enum;
  int counter_id;

  if (file_name == "memory.current") {
    counter_enum = protos::pbzero::CgroupCounters::CGROUP_MEMORY_CURRENT;
    counter_id = static_cast<int>(counter_enum);
  } else if (file_name == "memory.max") {
    counter_enum = protos::pbzero::CgroupCounters::CGROUP_MEMORY_MAX;
    counter_id = static_cast<int>(counter_enum);
  } else if (file_name == "memory.swap.current") {
    counter_enum = protos::pbzero::CgroupCounters::CGROUP_MEMORY_SWAP_CURRENT;
    counter_id = static_cast<int>(counter_enum);
  } else if (file_name == "memory.swap.max") {
    counter_enum = protos::pbzero::CgroupCounters::CGROUP_MEMORY_SWAP_MAX;
    counter_id = static_cast<int>(counter_enum);
  } else {
    return;
  }

  // Check if this counter is enabled by looking for its ID
  bool enabled = false;
  for (const auto& pair : cgroup_counters_) {
    if (pair.second == counter_id) {
      enabled = true;
      break;
    }
  }

  if (!enabled)
    return;

  auto value = static_cast<uint64_t>(strtoll(buf, nullptr, 10));
  auto* cgroup_value = sys_stats->add_cgroup();
  cgroup_value->set_key(counter_enum);
  cgroup_value->set_value(value);
  cgroup_value->set_cgroup_path(cgroup_path);
}

void SysStatsDataSource::ParseCgroupIoStat(protos::pbzero::SysStats* sys_stats,
                                           char* buf,
                                           size_t rsize,
                                           const std::string& cgroup_path) {
  for (base::StringSplitter lines(buf, rsize, '\n'); lines.Next();) {
    base::StringSplitter words(&lines, ' ');
    if (!words.Next())
      continue;

    std::string device = words.cur_token();

    // Parse IO stats: device rbytes=X wbytes=Y rios=Z wios=A dbytes=B dios=C
    while (words.Next()) {
      std::string token = words.cur_token();
      size_t eq_pos = token.find('=');
      if (eq_pos == std::string::npos)
        continue;

      std::string key = token.substr(0, eq_pos);
      std::string value_str = token.substr(eq_pos + 1);
      uint64_t value =
          static_cast<uint64_t>(strtoll(value_str.c_str(), nullptr, 10));

      protos::pbzero::CgroupCounters counter_enum;
      if (key == "rbytes") {
        counter_enum = protos::pbzero::CgroupCounters::CGROUP_IO_RBYTES;
      } else if (key == "wbytes") {
        counter_enum = protos::pbzero::CgroupCounters::CGROUP_IO_WBYTES;
      } else if (key == "rios") {
        counter_enum = protos::pbzero::CgroupCounters::CGROUP_IO_RIOS;
      } else if (key == "wios") {
        counter_enum = protos::pbzero::CgroupCounters::CGROUP_IO_WIOS;
      } else if (key == "dbytes") {
        counter_enum = protos::pbzero::CgroupCounters::CGROUP_IO_DBYTES;
      } else if (key == "dios") {
        counter_enum = protos::pbzero::CgroupCounters::CGROUP_IO_DIOS;
      } else {
        continue;
      }

      // Check if this counter is enabled by looking for its ID
      bool enabled = false;
      int counter_id = static_cast<int>(counter_enum);
      for (const auto& pair : cgroup_counters_) {
        if (pair.second == counter_id) {
          enabled = true;
          break;
        }
      }

      if (!enabled)
        continue;

      auto* cgroup_value = sys_stats->add_cgroup();
      cgroup_value->set_key(counter_enum);
      cgroup_value->set_value(value);
      cgroup_value->set_cgroup_path(cgroup_path);
      cgroup_value->set_device(device);
    }
  }
}

size_t SysStatsDataSource::ReadFile(base::ScopedFile* fd, const char* path) {
  if (!*fd)
    return 0;
  ssize_t res = pread(**fd, read_buf_.Get(), kReadBufSize - 1, 0);
  if (res <= 0) {
    PERFETTO_PLOG("Failed reading %s", path);
    fd->reset();
    return 0;
  }
  size_t rsize = static_cast<size_t>(res);
  static_cast<char*>(read_buf_.Get())[rsize] = '\0';
  return rsize + 1;  // Include null terminator in the count.
}

}  // namespace perfetto
