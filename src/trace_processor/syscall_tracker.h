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

#include <tuple>

#include "perfetto/base/string_view.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// Maximum syscall number known.
static constexpr size_t kSyscallCount = 330;

enum Architecture {
  kUnknown = 0,
  kAarch64,
  kX86_64,
};

class SyscallTracker {
 public:
  explicit SyscallTracker(TraceProcessorContext*);
  SyscallTracker(const SyscallTracker&) = delete;
  SyscallTracker& operator=(const SyscallTracker&) = delete;
  virtual ~SyscallTracker();

  void SetArchitecture(Architecture architecture);
  void Enter(int64_t ts, UniqueTid utid, uint32_t syscall_num);
  void Exit(int64_t ts, UniqueTid utid, uint32_t syscall_num);

 private:
  TraceProcessorContext* const context_;

  inline StringId SyscallNumberToStringId(uint32_t syscall) {
    if (syscall > kSyscallCount)
      return 0;
    // We see two write sys calls around each userspace slice that is going via
    // trace_marker, this violates the assumption that userspace slices are
    // perfectly nested. For the moment ignore all write sys calls.
    // TODO(hjd): Remove this limitation.
    StringId id = arch_syscall_to_string_id_[syscall];
    if (id == sys_write_string_id_)
      return 0;
    return id;
  }

  // This is table from platform specific syscall number directly to
  // the relevent StringId (this avoids having to always do two conversions).
  std::array<StringId, kSyscallCount> arch_syscall_to_string_id_;
  StringId sys_write_string_id_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SYSCALL_TRACKER_H_
