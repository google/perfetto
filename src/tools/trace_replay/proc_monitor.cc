/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/tools/trace_replay/proc_monitor.h"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/watchdog_posix.h"

namespace perfetto {
namespace trace_replay {

namespace {

// /proc/PID/statm is space-separated: size resident shared text lib data dt
// (all in pages).
bool ReadStatmRssPages(int pid, long* rss_pages, long* vm_pages) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/statm", pid);
  std::string buf;
  if (!base::ReadFile(path, &buf))
    return false;
  long sz = 0, rss = 0;
  if (sscanf(buf.c_str(), "%ld %ld", &sz, &rss) != 2)
    return false;
  *vm_pages = sz;
  *rss_pages = rss;
  return true;
}

}  // namespace

ProcMonitor::ProcMonitor(int pid, std::string csv_path, uint32_t interval_ms)
    : pid_(pid),
      csv_path_(std::move(csv_path)),
      interval_ms_(interval_ms ? interval_ms : 250) {}

ProcMonitor::~ProcMonitor() {
  Stop();
}

void ProcMonitor::Start() {
  stop_.store(false);
  thread_ = std::thread(&ProcMonitor::Run, this);
}

void ProcMonitor::Stop() {
  if (!thread_.joinable())
    return;
  stop_.store(true);
  thread_.join();
}

void ProcMonitor::Run() {
  base::ScopedFile out_fd(
      base::OpenFile(csv_path_, O_WRONLY | O_CREAT | O_TRUNC, 0644));
  if (!out_fd) {
    PERFETTO_ELOG("proc_monitor: cannot open %s: %s", csv_path_.c_str(),
                  strerror(errno));
    return;
  }
  const char* hdr = "t_ms,utime_ticks,stime_ticks,rss_kb,vm_kb\n";
  if (base::WriteAll(out_fd.get(), hdr, strlen(hdr)) < 0) {
    PERFETTO_ELOG("proc_monitor: write hdr failed");
    return;
  }

  const long page_kb = sysconf(_SC_PAGESIZE) / 1024;
  const auto t_start = std::chrono::steady_clock::now();
  while (!stop_.load(std::memory_order_relaxed)) {
    int64_t t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t_start)
                       .count();

    char stat_path[64];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid_);
    base::ScopedFile stat_fd(base::OpenFile(stat_path, O_RDONLY));
    if (!stat_fd) {
      // traced gone — stop polling.
      break;
    }
    base::ProcStat ps;
    if (!base::ReadProcStat(stat_fd.get(), &ps))
      break;

    long rss_pages = 0, vm_pages = 0;
    ReadStatmRssPages(pid_, &rss_pages, &vm_pages);
    long rss_kb = rss_pages * page_kb;
    long vm_kb = vm_pages * page_kb;
    long prev_peak = peak_rss_kb_.load(std::memory_order_relaxed);
    while (rss_kb > prev_peak &&
           !peak_rss_kb_.compare_exchange_weak(prev_peak, rss_kb,
                                               std::memory_order_relaxed)) {
    }

    char line[128];
    int n = snprintf(line, sizeof(line), "%" PRId64 ",%lu,%lu,%ld,%ld\n", t_ms,
                     ps.utime, ps.stime, rss_kb, vm_kb);
    if (n > 0 &&
        base::WriteAll(out_fd.get(), line, static_cast<size_t>(n)) < 0) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
  }
}

}  // namespace trace_replay
}  // namespace perfetto
