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
#include <string>
#include <vector>

#include "perfetto/base/time.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/thread_task_runner.h"

namespace perfetto {

class FtraceProcfs;

class KmemActivityTriggerThread {
 public:
  KmemActivityTriggerThread();
  ~KmemActivityTriggerThread();

 private:
  void InitializeOnThread();

  base::ThreadTaskRunner thread_;
  std::unique_ptr<FtraceProcfs> ftrace_procfs_;
  std::vector<base::ScopedFile> trace_pipe_fds_;
  base::TimeSeconds last_trigger_time_{0};
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_KMEM_ACTIVITY_TRIGGER_H_
