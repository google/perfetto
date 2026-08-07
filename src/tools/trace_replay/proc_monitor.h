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

#ifndef SRC_TOOLS_TRACE_REPLAY_PROC_MONITOR_H_
#define SRC_TOOLS_TRACE_REPLAY_PROC_MONITOR_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace perfetto {
namespace trace_replay {

class ProcMonitor {
 public:
  ProcMonitor(int pid, std::string csv_path, uint32_t interval_ms);
  ~ProcMonitor();
  void Start();
  void Stop();
  long peak_rss_kb() const {
    return peak_rss_kb_.load(std::memory_order_relaxed);
  }

 private:
  void Run();
  int pid_;
  std::string csv_path_;
  uint32_t interval_ms_;
  std::atomic<bool> stop_{false};
  std::atomic<long> peak_rss_kb_{0};
  std::thread thread_;
};

}  // namespace trace_replay
}  // namespace perfetto

#endif  // SRC_TOOLS_TRACE_REPLAY_PROC_MONITOR_H_
