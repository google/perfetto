/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_SYSCALL_TRACKER_H_
#define SRC_TRACE_PROCESSOR_SYSCALL_TRACKER_H_

#include <limits>
#include <tuple>

#include "perfetto/base/string_view.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

static constexpr size_t kMaxSyscalls = 550;

enum Architecture {
  kUnknown = 0,
  kArmEabi,  // 32-bit kernel running a 32-bit process (most old devices).
  kAarch32,  // 64-bit kernel running a 32-bit process (should be rare).
  kAarch64,  // 64-bit kernel running a 64-bit process (most new devices).
  kX86_64,
};

class SyscallTracker {
 public:
  explicit SyscallTracker(TraceProcessorContext*);
  SyscallTracker(const SyscallTracker&) = delete;
  SyscallTracker& operator=(const SyscallTracker&) = delete;
  virtual ~SyscallTracker();

  void SetArchitecture(Architecture architecture);

  void Enter(int64_t ts, UniqueTid utid, uint32_t syscall_num) {
    StringId name = SyscallNumberToStringId(syscall_num);
    if (name)
      context_->slice_tracker->Begin(ts, utid, 0 /* cat */, name);
  }

  void Exit(int64_t ts, UniqueTid utid, uint32_t syscall_num) {
    StringId name = SyscallNumberToStringId(syscall_num);
    if (name)
      context_->slice_tracker->End(ts, utid, 0 /* cat */, name);
  }

 private:
  TraceProcessorContext* const context_;

  inline StringId SyscallNumberToStringId(uint32_t syscall_num) {
    if (syscall_num > kMaxSyscalls)
      return 0;
    // We see two write sys calls around each userspace slice that is going via
    // trace_marker, this violates the assumption that userspace slices are
    // perfectly nested. For the moment ignore all write sys calls.
    // TODO(hjd): Remove this limitation.
    StringId id = arch_syscall_to_string_id_[syscall_num];
    if (id == sys_write_string_id_)
      return 0;
    return id;
  }

  // This is table from platform specific syscall number directly to
  // the relevent StringId (this avoids having to always do two conversions).
  std::array<StringId, kMaxSyscalls> arch_syscall_to_string_id_{};
  StringId sys_write_string_id_ = std::numeric_limits<StringId>::max();
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SYSCALL_TRACKER_H_
