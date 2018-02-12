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
#include <ctype.h>
#include <fstream>
#include <map>
#include <string>
#include "file_utils.h"
#include "process_info.h"
#include "procfs_utils.h"

int main(int argc, const char** argv) {
  if (argc != 1) {
    fprintf(stderr, "%s does not require any additional arguments.", argv[0]);
    return 1;
  }

  procfs_utils::ProcessMap processes;
  file_utils::ForEachPidInProcPath("/proc", [&processes](int pid) {
    if (!processes.count(pid)) {
      if (procfs_utils::ReadTgid(pid) != pid)
        return;
      processes[pid] = procfs_utils::ReadProcessInfo(pid);
    }
    ProcessInfo* process = processes[pid].get();
    procfs_utils::ReadProcessThreads(process);
  });

  procfs_utils::SerializeProcesses(&processes, stdout);
}
