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

#include "src/traced/probes/ftrace/ftrace_metadata.h"

namespace perfetto {

FtraceMetadata::FtraceMetadata() {
  // A lot of the time there will only be a small number of inodes.
  inode_and_device.reserve(10);
  pids.reserve(10);
}

void FtraceMetadata::AddDevice(BlockDeviceID device_id) {
  last_seen_device_id = device_id;
#if PERFETTO_DCHECK_IS_ON()
  seen_device_id = true;
#endif
}

void FtraceMetadata::AddInode(Inode inode_number) {
#if PERFETTO_DCHECK_IS_ON()
  PERFETTO_DCHECK(seen_device_id);
#endif
  static int32_t cached_pid = 0;
  if (!cached_pid)
    cached_pid = getpid();

  PERFETTO_DCHECK(last_seen_common_pid);
  PERFETTO_DCHECK(cached_pid == getpid());
  // Ignore own scanning activity.
  if (cached_pid != last_seen_common_pid) {
    inode_and_device.push_back(
        std::make_pair(inode_number, last_seen_device_id));
  }
}

void FtraceMetadata::AddCommonPid(int32_t pid) {
  last_seen_common_pid = pid;
}

void FtraceMetadata::AddPid(int32_t pid) {
  // Speculative optimization aginst repated pid's while keeping
  // faster insertion than a set.
  if (!pids.empty() && pids.back() == pid)
    return;
  pids.push_back(pid);
}

void FtraceMetadata::FinishEvent() {
  last_seen_device_id = 0;
#if PERFETTO_DCHECK_IS_ON()
  seen_device_id = false;
#endif
  last_seen_common_pid = 0;
}

void FtraceMetadata::Clear() {
  inode_and_device.clear();
  pids.clear();
  overwrite_count = 0;
  FinishEvent();
}

}  // namespace perfetto
