// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PROCESS_INFO_H_
#define PROCESS_INFO_H_

#include <map>

struct ThreadInfo {
  int tid;
  char name[16];
};

struct ProcessInfo {
  int pid;
  bool in_kernel;
  bool is_app;
  char name[256];
  char exe[256];
  std::map<int, ThreadInfo> threads;
};

#endif  // PROCESS_INFO_H_
