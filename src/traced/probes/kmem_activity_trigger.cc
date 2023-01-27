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

#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/waitable_event.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "src/traced/probes/probes_producer.h"

namespace perfetto {

namespace {
constexpr uint32_t kTriggerIntervalMs = 60 * 1000;  // 1 min.
constexpr size_t kPerCpuTraceBufferSizeInPages = 1;
constexpr char kTriggerName[] = "kmem_activity";

}  // namespace

// This is called by traced_probes' ProbesMain().
KmemActivityTrigger::KmemActivityTrigger()
    : task_runner_(base::ThreadTaskRunner::CreateAndStart()) {
  task_runner_.PostTask(
      [this]() { worker_data_.reset(new WorkerData(&task_runner_)); });
}

KmemActivityTrigger::~KmemActivityTrigger() {
  base::WaitableEvent evt;
  task_runner_.PostTask([this, &evt]() {
    worker_data_.reset();  // Destroy the WorkerData object.
    evt.Notify();
  });
  evt.Wait();
}

KmemActivityTrigger::WorkerData::~WorkerData() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (ftrace_procfs_) {
    ftrace_procfs_->SetTracingOn(false);
    ftrace_procfs_->ClearTrace();
  }
  DisarmFtraceFDWatches();
}

KmemActivityTrigger::WorkerData::WorkerData(base::TaskRunner* task_runner)
    : task_runner_(task_runner), weak_ptr_factory_(this) {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  ftrace_procfs_ =
      FtraceProcfs::CreateGuessingMountPoint("instances/mm_events/");
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
  ftrace_procfs_->DisableAllEvents();
  ftrace_procfs_->EnableEvent("vmscan", "mm_vmscan_direct_reclaim_begin");
  ftrace_procfs_->EnableEvent("compaction", "mm_compaction_begin");
  ftrace_procfs_->SetTracingOn(true);

  num_cpus_ = ftrace_procfs_->NumberOfCpus();
  for (size_t cpu = 0; cpu < num_cpus_; cpu++) {
    trace_pipe_fds_.emplace_back(ftrace_procfs_->OpenPipeForCpu(cpu));
    auto& scoped_fd = trace_pipe_fds_.back();
    if (!scoped_fd) {
      PERFETTO_PLOG("Failed to open trace_pipe_raw for cpu %zu", cpu);
      // Deliberately keeping this into the |trace_pipe_fds_| array so there is
      // a 1:1 mapping between CPU number and index in the array.
    } else {
      // Attempt reading from the trace pipe to detect if the CPU is disabled,
      // since open() doesn't fail. (b/169210648, b/178929757) This doesn't
      // block as OpenPipeForCpu() opens the pipe in non-blocking mode.
      char ch;
      if (base::Read(scoped_fd.get(), &ch, sizeof(char)) < 0 &&
          errno == ENODEV) {
        scoped_fd.reset();
      }
    }
  }

  ArmFtraceFDWatches();
}

void KmemActivityTrigger::WorkerData::ArmFtraceFDWatches() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  if (fd_watches_armed_)
    return;
  fd_watches_armed_ = true;
  for (size_t cpu = 0; cpu < trace_pipe_fds_.size(); cpu++) {
    const auto& scoped_fd = trace_pipe_fds_[cpu];
    if (!scoped_fd)
      continue;  // Can happen if the initial open() failed (CPU hotplug).
    ftrace_procfs_->ClearPerCpuTrace(cpu);
    task_runner_->AddFileDescriptorWatch(scoped_fd.get(), [weak_this, cpu] {
      if (weak_this)
        weak_this->OnFtracePipeWakeup(cpu);
    });
  }
}

void KmemActivityTrigger::WorkerData::DisarmFtraceFDWatches() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!fd_watches_armed_)
    return;
  fd_watches_armed_ = false;
  for (const base::ScopedFile& fd : trace_pipe_fds_) {
    if (fd)
      task_runner_->RemoveFileDescriptorWatch(fd.get());
  }
}

void KmemActivityTrigger::WorkerData::OnFtracePipeWakeup(size_t cpu) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("KmemActivityTrigger ftrace pipe wakeup on cpu %zu", cpu);
  ftrace_procfs_->ClearPerCpuTrace(cpu);

  if (!fd_watches_armed_) {
    // If false, another task for another CPU got here, disarmed the watches
    // and posted the re-arming. Don't append another task.
    return;
  }

  ProbesProducer* probes_producer = ProbesProducer::GetInstance();
  if (probes_producer)
    probes_producer->ActivateTrigger(kTriggerName);

  // Once a ftrace pipe wakes up, disarm the poll() and re-enable only after
  // kTriggerIntervalMs. This is to avoid spinning on the pipes if there is too
  // much ftrace activity (b/178929757).

  DisarmFtraceFDWatches();

  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this] {
        if (weak_this)
          weak_this->ArmFtraceFDWatches();
      },
      kTriggerIntervalMs);
}

}  // namespace perfetto
