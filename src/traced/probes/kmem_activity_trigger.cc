/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/traced/probes/kmem_activity_trigger.h"

#include <unistd.h>

#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "src/traced/probes/probes_producer.h"

namespace perfetto {

namespace {
constexpr base::TimeSeconds kTriggerInterval{60};  // 1 min
constexpr size_t kPerCpuTraceBufferSizeInPages = 1;
constexpr char kTriggerName[] = "kmem_activity";
}  // namespace

void KmemActivityTriggerThread::InitializeOnThread() {
  // Create kmem activity FtraceProcfs
  size_t index = 0;
  while (!ftrace_procfs_ && FtraceController::kTracingPaths[index]) {
    std::string root = FtraceController::kTracingPaths[index++] +
                       std::string("instances/mm_events/");
    ftrace_procfs_ = FtraceProcfs::Create(root);
  }
  if (!ftrace_procfs_) {
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
    PERFETTO_LOG(
        "mm_events ftrace instance not found. Triggering of traces on memory "
        "pressure will not be available on this device.");
#endif
    return;
  }

  ftrace_procfs_->SetCpuBufferSizeInPages(kPerCpuTraceBufferSizeInPages);

  // Enable mm trace events
  ftrace_procfs_->EnableEvent("vmscan", "mm_vmscan_kswapd_wake");
  ftrace_procfs_->EnableEvent("vmscan", "mm_vmscan_direct_reclaim_begin");
  ftrace_procfs_->EnableEvent("compaction", "mm_compaction_begin");

  ftrace_procfs_->EnableTracing();

  size_t num_cpus = ftrace_procfs_->NumberOfCpus();
  for (size_t cpu = 0; cpu < num_cpus; cpu++) {
    trace_pipe_fds_.emplace_back(ftrace_procfs_->OpenPipeForCpu(cpu));

    if (!trace_pipe_fds_.back()) {
      PERFETTO_PLOG("Failed to open trace_pipe_raw for cpu %zu", cpu);
      trace_pipe_fds_.pop_back();
      continue;
    }

    int watch_fd = trace_pipe_fds_.back().get();
    thread_.AddFileDescriptorWatch(watch_fd, [this, cpu]() {
      base::TimeSeconds now = base::GetBootTimeS();
      base::TimeSeconds elapsed = std::chrono::duration_cast<base::TimeSeconds>(
          now - last_trigger_time_);
      ProbesProducer* probes_producer = ProbesProducer::GetInstance();
      if (probes_producer &&
          (elapsed > kTriggerInterval || last_trigger_time_.count() == 0)) {
        probes_producer->ActivateTrigger(kTriggerName);
        last_trigger_time_ = now;
      }
      ftrace_procfs_->ClearPerCpuTrace(cpu);
    });
  }
}

KmemActivityTriggerThread::KmemActivityTriggerThread()
    : thread_(base::ThreadTaskRunner::CreateAndStart()) {
  thread_.PostTask([this]() { InitializeOnThread(); });
}

KmemActivityTriggerThread::~KmemActivityTriggerThread() {
  thread_.PostTask([this]() {
    ftrace_procfs_->DisableTracing();
    ftrace_procfs_->ClearTrace();

    for (const base::ScopedFile& fd : trace_pipe_fds_) {
      thread_.RemoveFileDescriptorWatch(fd.get());
    }

    trace_pipe_fds_.clear();

    thread_.get()->Quit();
  });
}

}  // namespace perfetto
