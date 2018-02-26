// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_PROCESS_STATS_PROCESS_INFO_H_
#define SRC_PROCESS_STATS_PROCESS_INFO_H_

#include <map>
#include <vector>

struct ThreadInfo {
  int tid;
  char name[16];
};

struct ProcessInfo {
  int pid;
  int ppid;
  bool in_kernel;
  bool is_app;
  char exe[256];
  std::vector<std::string> cmdline;
  std::map<int, ThreadInfo> threads;
};

#endif  // SRC_PROCESS_STATS_PROCESS_INFO_H_
