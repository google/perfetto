/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "perfetto/ext/base/sched.h"

#include "perfetto/base/build_config.h"
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <linux/capability.h>
#include <sched.h>  // Used only for SCHED_ macros
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <optional>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto::base {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
namespace {
constexpr pid_t kCurrentPid = 0;

StatusOr<SchedConfig::SchedPolicy> SchedPolicyFromCApi(
    const unsigned int policy) {
  switch (policy) {
    case SCHED_OTHER:
      return SchedConfig::SchedPolicy::kOther;
    case SCHED_BATCH:
      return SchedConfig::SchedPolicy::kBatch;
    case SCHED_IDLE:
      return SchedConfig::SchedPolicy::kIdle;
    case SCHED_FIFO:
      return SchedConfig::SchedPolicy::kFifo;
    case SCHED_RR:
      return SchedConfig::SchedPolicy::kRr;
    default:
      // TODO(ktimofeev): support SCHED_DEADLINE
      return ErrStatus("Unknown C API sched policy %d", policy);
  }
}

unsigned int SchedPolicyToCApi(const SchedConfig::SchedPolicy& policy) {
  switch (policy) {
    case SchedConfig::SchedPolicy::kIdle:
      return SCHED_IDLE;
    case SchedConfig::SchedPolicy::kOther:
      return SCHED_OTHER;
    case SchedConfig::SchedPolicy::kBatch:
      return SCHED_BATCH;
    case SchedConfig::SchedPolicy::kFifo:
      return SCHED_FIFO;
    case SchedConfig::SchedPolicy::kRr:
      return SCHED_RR;
  }
  PERFETTO_FATAL("Can't be here (Unknown sched policy enum value %d)",
                 static_cast<int>(policy));
}
}  // namespace

std::string SchedConfig::ToString() const {
  const std::string kernel_info =
      "kernel_policy=" + std::to_string(SchedPolicyToCApi(policy_)) +
      ", kernel_prio=" + std::to_string(KernelPriority());
  switch (policy_) {
    case SchedPolicy::kIdle:
      return "IDLE(" + kernel_info + ")";
    case SchedPolicy::kOther:
      return "OTHER(nice=" + std::to_string(nice_) + ", " + kernel_info + ")";
    case SchedPolicy::kBatch:
      return "BATCH(nice=" + std::to_string(nice_) + ", " + kernel_info + ")";
    case SchedPolicy::kFifo:
      return "FIFO(priority=" + std::to_string(rt_priority_) + ", " +
             kernel_info + ")";
    case SchedPolicy::kRr:
      return "RR(priority=" + std::to_string(rt_priority_) + ", " +
             kernel_info + ")";
  }
  PERFETTO_FATAL("Can't be here (Unknown sched policy enum value %d)",
                 static_cast<int>(policy_));
}
unsigned int SchedConfig::KernelPolicy() const {
  return SchedPolicyToCApi(policy_);
}

std::optional<LinuxProcSelfStatSchedInfo> ReadProcSelfStatSchedInfo() {
  std::string stat;
  if (!ReadFile("/proc/self/stat", &stat)) {
    return std::nullopt;
  }
  // The stat file is a single line split into space-separated fields as "pid
  // (comm) state ppid ...". However because the command name can contain any
  // characters (including parentheses and spaces), we need to skip past it
  // before parsing the rest of the fields. To do that, we look for the last
  // instance of ") " (parentheses followed by space) and parse forward from
  // that point.
  // (Copied from 'src/tracing/track.cc')
  // TODO(ktimofeev): extract this logic to some place in 'base/'
  size_t comm_end = stat.rfind(") ");
  if (comm_end == std::string::npos) {
    return std::nullopt;
  }
  stat = stat.substr(comm_end + strlen(") "));
  // Now we need to subtract two from each index in the stat array
  constexpr int kOneIndexOffset =
      1 /* starts from one */ + 2 /* skips pid and proc name*/;
  const auto& parts = SplitString(stat, " ");
  if (parts.size() <=
      LinuxProcSelfStatSchedInfo::kPolicyIdx - kOneIndexOffset) {
    return std::nullopt;
  }
  const auto priority = StringToInt32(
      parts[LinuxProcSelfStatSchedInfo::kPriorityIdx - kOneIndexOffset]);
  const auto nice = StringToInt32(
      parts[LinuxProcSelfStatSchedInfo::kNiceIdx - kOneIndexOffset]);
  const auto rt_priority = StringToUInt32(
      parts[LinuxProcSelfStatSchedInfo::kRtPriorityIdx - kOneIndexOffset]);
  const auto policy = StringToUInt32(
      parts[LinuxProcSelfStatSchedInfo::kPolicyIdx - kOneIndexOffset]);
  if (!priority.has_value() || !nice.has_value() || !rt_priority.has_value() ||
      !policy.has_value()) {
    return std::nullopt;
  }
  return LinuxProcSelfStatSchedInfo{priority.value(), nice.value(),
                                    rt_priority.value(), policy.value()};
}

SchedManagerInterface::~SchedManagerInterface() = default;

bool SchedManager::IsSupportedOnTheCurrentPlatform() const {
  return true;
}

bool SchedManager::HasCapabilityToSetSchedPolicy() const {
  __user_cap_header_struct header{};
  header.version = _LINUX_CAPABILITY_VERSION_3;
  header.pid = kCurrentPid;
  __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3] = {};
  // Don't want to add a build dependency on a libcap(3), so use raw syscall.
  if (syscall(__NR_capget, &header, data) == -1) {
    PERFETTO_DFATAL_OR_ELOG("Failed to call capget (errno: %d, %s)", errno,
                            strerror(errno));
    return false;
  }

  constexpr int capability = CAP_SYS_NICE;
  constexpr int index = CAP_TO_INDEX(capability);
  constexpr unsigned int mask = CAP_TO_MASK(capability);

  return (data[index].effective & mask) != 0;
}

namespace {
// 'sched_attr' struct (together with sched_setattr and sched_getattr wrapper
// functions) was added to the glibc version 2.41. To support older libc
// versions, we define the struct ourselves and use raw syscalls.
// See b/183240349 on the sched_attr support in bionic.
// Struct definition copied from
// https://github.com/torvalds/linux/blob/11313e2f78128c948e9b4eb58b3dacfc30964700/include/uapi/linux/sched/types.h#L98
struct sched_attr_redefined {
  __u32 size;

  __u32 sched_policy;
  __u64 sched_flags;

  /* SCHED_NORMAL, SCHED_BATCH */
  __s32 sched_nice;

  /* SCHED_FIFO, SCHED_RR */
  __u32 sched_priority;

  /* SCHED_DEADLINE */
  __u64 sched_runtime;
  __u64 sched_deadline;
  __u64 sched_period;

  /* Utilization hints */
  __u32 sched_util_min;
  __u32 sched_util_max;
};

// Define our own version if not defined in <sched.h>
#ifndef SCHED_FLAG_RESET_ON_FORK
#define SCHED_FLAG_RESET_ON_FORK 0x01
#endif
}  // namespace

StatusOr<SchedConfig> SchedManager::GetCurrentSchedConfig() const {
  sched_attr_redefined attrs{};
  if (const int ret = static_cast<int>(
          syscall(__NR_sched_getattr, kCurrentPid, &attrs, sizeof(attrs), 0));
      ret < 0) {
    return ErrStatus("Cannot get current scheduler info (errno: %d, %s)", errno,
                     strerror(errno));
  }
  const auto& policy = SchedPolicyFromCApi(attrs.sched_policy);
  if (!policy.ok()) {
    return ErrStatus("Cannot get current scheduler info: %s",
                     policy.status().c_message());
  }
  if (policy.value() == SchedConfig::SchedPolicy::kIdle) {
    // For 'SCHED_IDLE' 'sched_getattr' doesn't set 'sched_nice' to zero.
    attrs.sched_nice = 0;
  }
  return SchedConfig(policy.value(), attrs.sched_priority, attrs.sched_nice);
}

Status SchedManager::SetSchedConfig(const SchedConfig& arg) {
  sched_attr_redefined attrs{};
  attrs.size = sizeof(sched_attr_redefined);
  attrs.sched_policy = SchedPolicyToCApi(arg.policy());
  attrs.sched_priority = arg.priority();
  attrs.sched_nice = arg.nice();
  attrs.sched_flags = SCHED_FLAG_RESET_ON_FORK;  // Children created by fork(2)
                                                 // do not inherit privileged
                                                 // scheduling policies.
  if (const int ret =
          static_cast<int>(syscall(__NR_sched_setattr, kCurrentPid, &attrs, 0));
      ret < 0) {
    return ErrStatus("Cannot set scheduler policy (errno: %d, %s)", errno,
                     strerror(errno));
  }
  return OkStatus();
}
#else

unsigned int SchedConfig::KernelPolicy() const {
  return 0;
}

std::string SchedConfig::ToString() const {
  return "";
}

std::optional<LinuxProcSelfStatSchedInfo> ReadProcSelfStatSchedInfo() {
  return std::nullopt;
}

bool SchedManager::IsSupportedOnTheCurrentPlatform() const {
  return false;
}

bool SchedManager::HasCapabilityToSetSchedPolicy() const {
  return false;
}

Status SchedManager::SetSchedConfig(const SchedConfig&) {
  return ErrStatus("SetSchedConfig() not implemented on the current platform");
}

StatusOr<SchedConfig> SchedManager::GetCurrentSchedConfig() const {
  return ErrStatus(
      "GetCurrentSchedConfig() not implemented on the current platform");
}

#endif
}  // namespace perfetto::base
