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

#ifndef SRC_TRACED_PROBES_FTRACE_FTRACE_THREAD_SYNC_H_
#define SRC_TRACED_PROBES_FTRACE_FTRACE_THREAD_SYNC_H_

#include <stdint.h>

#include <bitset>
#include <condition_variable>
#include <mutex>

#include "perfetto/base/utils.h"
#include "perfetto/base/weak_ptr.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base

class FtraceController;

// This struct is accessed both by the FtraceController on the main thread and
// by the CpuReader(s) on their worker threads. It is used to synchronize
// handshakes between FtraceController and CpuReader(s). There is only *ONE*
// instance of this state, owned by the FtraceController and shared with all
// CpuReader(s).
struct FtraceThreadSync {
  explicit FtraceThreadSync(base::TaskRunner* tr) : task_runner(tr) {}

  // These variables are set upon initialization time and never changed. Can
  // be accessed outside of the |mutex|.
  base::TaskRunner* const task_runner;  // Where the FtraceController lives.
  base::WeakPtr<FtraceController> trace_controller_weak;

  // Mutex & condition variable shared by main thread and all per-cpu workers.
  // All fields below are read and modified holding |mutex|.
  std::mutex mutex;

  // Used to suspend CpuReader(s) between cycles and to wake them up at the
  // same time.
  std::condition_variable cond;

  // |cmd| and |cmd_id| are written only by FtraceController. On each cycle,
  // FtraceController increases the |cmd_id| monotonic counter and issues the
  // new command. |cmd_id| is used by the CpuReader(s) to distinguish a new
  // command from a spurious wakeup.
  enum Cmd { kRun = 0, kFlush, kQuit };
  Cmd cmd = kRun;
  uint64_t cmd_id = 0;

  // This bitmap is cleared by the FtraceController before every kRun command
  // and is optionally set by OnDataAvailable() if a CpuReader did fetch any
  // ftrace data during the read cycle.
  std::bitset<base::kMaxCpus> cpus_to_drain;

  // This bitmap is cleared by the FtraceController before issuing a kFlush
  // command and set by each CpuReader after they have completed the flush.
  std::bitset<base::kMaxCpus> flush_acks;
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_FTRACE_THREAD_SYNC_H_
