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

#ifndef SRC_TRACED_PROBES_KMEM_ACTIVITY_TRIGGER_H_
#define SRC_TRACED_PROBES_KMEM_ACTIVITY_TRIGGER_H_

#include <memory>
#include <vector>

#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/thread_task_runner.h"
#include "perfetto/ext/base/weak_ptr.h"

namespace perfetto {

class FtraceProcfs;

class KmemActivityTrigger {
 public:
  KmemActivityTrigger();
  ~KmemActivityTrigger();

 private:
  // This object lives entirely on the KmemActivityTrigger |task_runner_|.
  class WorkerData {
   public:
    WorkerData(base::TaskRunner*);
    ~WorkerData();
    void InitializeOnThread();
    void ArmFtraceFDWatches();
    void DisarmFtraceFDWatches();
    void OnFtracePipeWakeup(size_t cpu);

   private:
    // All the fields below are accessed only on the dedicated |task_runner_|.
    base::TaskRunner* const task_runner_;
    std::unique_ptr<FtraceProcfs> ftrace_procfs_;
    std::vector<base::ScopedFile> trace_pipe_fds_;
    size_t num_cpus_ = 0;
    bool fd_watches_armed_ = false;

    // Keep last.
    base::WeakPtrFactory<WorkerData> weak_ptr_factory_;
    PERFETTO_THREAD_CHECKER(thread_checker_)
  };

  base::ThreadTaskRunner task_runner_;
  std::unique_ptr<WorkerData> worker_data_;
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_KMEM_ACTIVITY_TRIGGER_H_
