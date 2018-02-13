// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "procfs_utils.h"

#include <stdio.h>
#include <string.h>
#include <fstream>

#include "file_utils.h"

using file_utils::ForEachPidInProcPath;
using file_utils::ReadProcFile;
using file_utils::ReadProcFileTrimmed;

namespace procfs_utils {

namespace {

const char kJavaAppPrefix[] = "/system/bin/app_process";
const char kZygotePrefix[] = "zygote";

inline void ReadProcString(int pid, const char* path, char* buf, size_t size) {
  if (!file_utils::ReadProcFileTrimmed(pid, path, buf, size))
    buf[0] = '\0';
}

inline void ReadExePath(int pid, char* buf, size_t size) {
  char exe_path[64];
  sprintf(exe_path, "/proc/%d/exe", pid);
  ssize_t res = readlink(exe_path, buf, size - 1);
  if (res >= 0)
    buf[res] = '\0';
  else
    buf[0] = '\0';
}

inline bool IsApp(const char* name, const char* exe) {
  return strncmp(exe, kJavaAppPrefix, sizeof(kJavaAppPrefix) - 1) == 0 &&
         strncmp(name, kZygotePrefix, sizeof(kZygotePrefix) - 1) != 0;
}

}  // namespace

int ReadTgid(int pid) {
  static const char kTgid[] = "\nTgid:";
  char buf[512];
  ssize_t rsize = ReadProcFile(pid, "status", buf, sizeof(buf));
  if (rsize <= 0)
    return -1;
  const char* tgid_line = strstr(buf, kTgid);
  return atoi(tgid_line + sizeof(kTgid) - 1);
}

std::unique_ptr<ProcessInfo> ReadProcessInfo(int pid) {
  ProcessInfo* process = new ProcessInfo();
  process->pid = pid;
  ReadProcString(pid, "cmdline", process->name, sizeof(process->name));
  if (process->name[0] != 0) {
    ReadExePath(pid, process->exe, sizeof(process->exe));
    process->is_app = IsApp(process->name, process->exe);
  } else {
    ReadProcString(pid, "comm", process->name, sizeof(process->name));
    process->in_kernel = true;
  }
  return std::unique_ptr<ProcessInfo>(process);
}

void ReadProcessThreads(ProcessInfo* process) {
  if (process->in_kernel)
    return;

  char tasks_path[64];
  sprintf(tasks_path, "/proc/%d/task", process->pid);
  ForEachPidInProcPath(tasks_path, [process](int tid) {
    if (process->threads.count(tid))
      return;
    ThreadInfo thread = {tid, ""};
    char task_comm[64];
    sprintf(task_comm, "task/%d/comm", tid);
    ReadProcString(process->pid, task_comm, thread.name, sizeof(thread.name));
    if (thread.name[0] == '\0' && process->is_app)
      strcpy(thread.name, "UI Thread");
    process->threads[tid] = thread;
  });
}

void SerializeProcesses(ProcessMap* processes, FILE* out) {
  fprintf(out, "\"processes\":{");
  for (auto it = processes->begin(); it != processes->end();) {
    const ProcessInfo* process = it->second.get();
    fprintf(out, "\"%d\":{", process->pid);
    fprintf(out, "\"name\":\"%s\"", process->name);

    if (!process->in_kernel) {
      fprintf(out, ",\"exe\":\"%s\",", process->exe);
      fprintf(out, "\"threads\":{\n");
      const auto threads = &process->threads;
      for (auto thread_it = threads->begin(); thread_it != threads->end();) {
        const ThreadInfo* thread = &(thread_it->second);
        fprintf(out, "\"%d\":{", thread->tid);
        fprintf(out, "\"name\":\"%s\"", thread->name);

        if (++thread_it != threads->end())
          fprintf(out, "},\n");
        else
          fprintf(out, "}\n");
      }
      fprintf(out, "}");
    }

    if (++it != processes->end())
      fprintf(out, "},\n");
    else
      fprintf(out, "}\n");
  }
  fprintf(out, "}");
}

}  // namespace procfs_utils
