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
#include <limits>
#include <utility>

#include "perfetto/base/file_utils.h"
#include "perfetto/base/metatrace.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/string_splitter.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/base/utils.h"
#include "perfetto/traced/sys_stats_counters.h"
#include "perfetto/tracing/core/sys_stats_config.h"

#include "perfetto/common/sys_stats_counters.pbzero.h"
#include "perfetto/config/sys_stats/sys_stats_config.pb.h"
#include "perfetto/trace/sys_stats/sys_stats.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {
constexpr size_t kReadBufSize = 1024 * 16;

base::ScopedFile OpenReadOnly(const char* path) {
  base::ScopedFile fd;
  fd.reset(open(path, O_RDONLY | O_CLOEXEC));
  if (!fd)
    PERFETTO_PLOG("Failed opening %s", path);
  return fd;
}

}  // namespace

// static
constexpr int SysStatsDataSource::kTypeId;

SysStatsDataSource::SysStatsDataSource(base::TaskRunner* task_runner,
                                       TracingSessionID session_id,
                                       std::unique_ptr<TraceWriter> writer,
                                       const DataSourceConfig& ds_config,
                                       OpenFunction open_fn)
    : ProbesDataSource(session_id, kTypeId),
      task_runner_(task_runner),
      writer_(std::move(writer)),
      weak_factory_(this) {
  const auto& config = ds_config.sys_stats_config();

  ns_per_user_hz_ = 1000000000ull / static_cast<uint64_t>(sysconf(_SC_CLK_TCK));

  open_fn = open_fn ? open_fn : OpenReadOnly;
  meminfo_fd_ = open_fn("/proc/meminfo");
  vmstat_fd_ = open_fn("/proc/vmstat");
  stat_fd_ = open_fn("/proc/stat");

  read_buf_ = base::PageAllocator::Allocate(kReadBufSize);

  // Build a lookup map that allows to quickly translate strings like "MemTotal"
  // into the corresponding enum value, only for the counters enabled in the
  // config.
  for (const auto& counter_id : config.meminfo_counters()) {
    for (size_t i = 0; i < base::ArraySize(kMeminfoKeys); i++) {
      const auto& k = kMeminfoKeys[i];
      if (static_cast<int>(k.id) == static_cast<int>(counter_id))
        meminfo_counters_.emplace(k.str, k.id);
    }
  }

  for (const auto& counter_id : config.vmstat_counters()) {
    for (size_t i = 0; i < base::ArraySize(kVmstatKeys); i++) {
      const auto& k = kVmstatKeys[i];
      if (static_cast<int>(k.id) == static_cast<int>(counter_id))
        vmstat_counters_.emplace(k.str, k.id);
    }
  }

  for (const auto& counter_id : config.stat_counters()) {
    stat_enabled_fields_ |= 1 << counter_id;
  }

  std::array<uint32_t, 3> periods_ms{};
  std::array<uint32_t, 3> ticks{};
  static_assert(periods_ms.size() == ticks.size(), "must have same size");
  periods_ms[0] = config.meminfo_period_ms();
  periods_ms[1] = config.vmstat_period_ms();
  periods_ms[2] = config.stat_period_ms();
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
  uint32_t delay_ms = period_ms - (base::GetWallTimeMs().count() % period_ms);
  thiz.task_runner_->PostDelayedTask(
      std::bind(&SysStatsDataSource::Tick, weak_this), delay_ms);
  thiz.ReadSysStats();
}

SysStatsDataSource::~SysStatsDataSource() = default;

void SysStatsDataSource::ReadSysStats() {
  PERFETTO_METATRACE("ReadSysStats", 0);
  auto packet = writer_->NewTracePacket();

  packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));
  auto* sys_stats = packet->set_sys_stats();

  if (meminfo_ticks_ && tick_ % meminfo_ticks_ == 0)
    ReadMeminfo(sys_stats);

  if (vmstat_ticks_ && tick_ % vmstat_ticks_ == 0)
    ReadVmstat(sys_stats);

  if (stat_ticks_ && tick_ % stat_ticks_ == 0)
    ReadStat(sys_stats);

  tick_++;
}

void SysStatsDataSource::ReadMeminfo(protos::pbzero::SysStats* sys_stats) {
  size_t rsize = ReadFile(&meminfo_fd_, "/proc/meminfo");
  if (!rsize)
    return;
  char* buf = static_cast<char*>(read_buf_.get());
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
  char* buf = static_cast<char*>(read_buf_.get());
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
  char* buf = static_cast<char*>(read_buf_.get());
  for (base::StringSplitter lines(buf, rsize, '\n'); lines.Next();) {
    base::StringSplitter words(&lines, ' ');
    if (!words.Next())
      continue;

    // Per-CPU stats.
    if ((stat_enabled_fields_ & (1 << SysStatsConfig::STAT_CPU_TIMES)) &&
        words.cur_token_size() > 3 && !strncmp(words.cur_token(), "cpu", 3)) {
      long cpu_id = strtol(words.cur_token() + 3, nullptr, 10);
      std::array<uint64_t, 7> cpu_times{};
      for (size_t i = 0; i < cpu_times.size() && words.Next(); i++) {
        cpu_times[i] =
            static_cast<uint64_t>(strtoll(words.cur_token(), nullptr, 10));
      }
      auto* cpu_stat = sys_stats->add_cpu_stat();
      cpu_stat->set_cpu_id(static_cast<uint32_t>(cpu_id));
      cpu_stat->set_user_ns(cpu_times[0] * ns_per_user_hz_);
      cpu_stat->set_user_ice_ns(cpu_times[1] * ns_per_user_hz_);
      cpu_stat->set_system_mode_ns(cpu_times[2] * ns_per_user_hz_);
      cpu_stat->set_idle_ns(cpu_times[3] * ns_per_user_hz_);
      cpu_stat->set_io_wait_ns(cpu_times[4] * ns_per_user_hz_);
      cpu_stat->set_irq_ns(cpu_times[5] * ns_per_user_hz_);
      cpu_stat->set_softirq_ns(cpu_times[6] * ns_per_user_hz_);
    }
    // IRQ counters
    else if ((stat_enabled_fields_ & (1 << SysStatsConfig::STAT_IRQ_COUNTS)) &&
             !strcmp(words.cur_token(), "intr")) {
      for (size_t i = 0; words.Next(); i++) {
        auto v = static_cast<uint64_t>(strtoll(words.cur_token(), nullptr, 10));
        if (i == 0) {
          sys_stats->set_num_irq_total(v);
        } else {
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

void SysStatsDataSource::Flush() {
  writer_->Flush();
}

size_t SysStatsDataSource::ReadFile(base::ScopedFile* fd, const char* path) {
  if (!*fd)
    return 0;
  ssize_t res = pread(**fd, read_buf_.get(), kReadBufSize - 1, 0);
  if (res <= 0) {
    PERFETTO_PLOG("Failed reading %s", path);
    fd->reset();
    return 0;
  }
  size_t rsize = static_cast<size_t>(res);
  static_cast<char*>(read_buf_.get())[rsize] = '\0';
  return rsize + 1;  // Include null terminator in the count.
}

}  // namespace perfetto
