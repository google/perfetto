/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_TRACED_PROBES_FTRACE_FTRACE_METADATA_H_
#define SRC_TRACED_PROBES_FTRACE_FTRACE_METADATA_H_

#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>
#include <vector>

#include "perfetto/base/logging.h"

namespace perfetto {

using BlockDeviceID = decltype(stat::st_dev);
using Inode = decltype(stat::st_ino);

struct FtraceMetadata {
  FtraceMetadata();

  uint32_t overwrite_count;
  BlockDeviceID last_seen_device_id = 0;
#if PERFETTO_DCHECK_IS_ON()
  bool seen_device_id = false;
#endif
  int32_t last_seen_common_pid = 0;

  // A vector not a set to keep the writer_fast.
  std::vector<std::pair<Inode, BlockDeviceID>> inode_and_device;
  std::vector<int32_t> pids;

  void AddDevice(BlockDeviceID);
  void AddInode(Inode);
  void AddPid(int32_t);
  void AddCommonPid(int32_t);
  void Clear();
  void FinishEvent();
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_FTRACE_METADATA_H_
