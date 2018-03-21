// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/process_stats/procfs_utils.h"

#include <stdio.h>
#include <string.h>
#include <fstream>

#include "file_utils.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/string_splitter.h"

using file_utils::ForEachPidInProcPath;
using file_utils::ReadProcFile;
using file_utils::ReadProcFileTrimmed;

namespace procfs_utils {

namespace {

constexpr const char kJavaAppPrefix[] = "/system/bin/app_process";
constexpr const char kZygotePrefix[] = "zygote";

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

inline int ReadStatusLine(int pid, const char* status_string) {
  char buf[512];
  ssize_t rsize = ReadProcFile(pid, "status", buf, sizeof(buf));
  if (rsize <= 0)
    return -1;
  const char* line = strstr(buf, status_string);
  PERFETTO_DCHECK(line);
  return atoi(line + strlen(status_string));
}

}  // namespace

int ReadTgid(int pid) {
  return ReadStatusLine(pid, "\nTgid:");
}

int ReadPpid(int pid) {
  return ReadStatusLine(pid, "\nPPid:");
}

std::unique_ptr<ProcessInfo> ReadProcessInfo(int pid) {
  ProcessInfo* process = new ProcessInfo();
  process->pid = pid;
  char cmdline_buf[256];
  ReadProcString(pid, "cmdline", cmdline_buf, sizeof(cmdline_buf));
  if (cmdline_buf[0] == 0) {
    // Nothing in cmdline_buf so read name from /comm instead.
    char name[256];
    ReadProcString(pid, "comm", name, sizeof(name));
    process->cmdline.push_back(name);
    process->in_kernel = true;
  } else {
    using perfetto::base::StringSplitter;
    for (StringSplitter ss(cmdline_buf, sizeof(cmdline_buf), '\0'); ss.Next();)
      process->cmdline.push_back(ss.cur_token());
    ReadExePath(pid, process->exe, sizeof(process->exe));
    process->is_app = IsApp(process->cmdline[0].c_str(), process->exe);
  }
  process->ppid = ReadPpid(pid);
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

}  // namespace procfs_utils
