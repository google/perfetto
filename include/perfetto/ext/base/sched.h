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

#ifndef INCLUDE_PERFETTO_EXT_BASE_SCHED_H_
#define INCLUDE_PERFETTO_EXT_BASE_SCHED_H_

#include "perfetto/base/build_config.h"

#include <string>
#include <tuple>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/ext/base/status_or.h"

namespace perfetto::base {

struct SchedConfig {
  enum class SchedPolicy { kIdle, kBatch, kOther, kFifo, kRr };
  static SchedConfig CreateIdle() { return {SchedPolicy::kIdle, 0, 0}; }
  static SchedConfig CreateDefaultUserspacePolicy() { return CreateOther(0); }
  static SchedConfig CreateOther(int nice) {
    return {SchedPolicy::kOther, 0, nice};
  }
  static SchedConfig CreateBatch(int nice) {
    return {SchedPolicy::kBatch, 0, nice};
  }
  static SchedConfig CreateFifo(unsigned int priority) {
    return {SchedPolicy::kFifo, priority, 0};
  }
  static SchedConfig CreateRr(unsigned int priority) {
    return {SchedPolicy::kRr, priority, 0};
  }

  SchedConfig(const SchedPolicy policy,
              const unsigned int priority,
              const int nice)
      : policy_(policy), rt_priority_(priority), nice_(nice) {
    switch (policy) {
      case SchedPolicy::kIdle:
        PERFETTO_DCHECK(priority == 0 && nice == 0);
        break;
      case SchedPolicy::kOther:
      case SchedPolicy::kBatch:
        PERFETTO_DCHECK(ValidateNiceValue(nice).ok() && priority == 0);
        break;
      case SchedPolicy::kFifo:
      case SchedPolicy::kRr:
        PERFETTO_DCHECK(ValidatePriority(priority).ok() && nice == 0);
        break;
    }
  }

  std::string ToString() const;

  /**
   * Return true if this priority is _lower_ than others'
   * (that implies `this.KernelPriority()` is _higher_ than
   * `other.KernelPriority()`).
   *
   * If both priorities have the same `KernelPriority()` value, we compare the
   * policies.
   *
   * Strictly speaking, it is not correct to compare realtime priorities
   * 'SCHED_FIFO' and 'SCHED_RR' based on their policy, but we do it for
   * simplicity.
   */
  bool operator<(const SchedConfig& other) const {
    const int inverted_priority =
        -1 * static_cast<int>(InternalUnifiedPriority());
    const int inverted_other_priority =
        -1 * static_cast<int>(other.InternalUnifiedPriority());
    return std::tie(inverted_priority, policy_) <
           std::tie(inverted_other_priority, other.policy_);
  }

  bool operator==(const SchedConfig& other) const {
    return std::tie(policy_, rt_priority_, nice_) ==
           std::tie(other.policy_, other.rt_priority_, other.nice_);
  }
  bool operator!=(const SchedConfig& other) const { return !(*this == other); }

  SchedPolicy policy() const { return policy_; }
  unsigned int priority() const { return rt_priority_; }
  int nice() const { return nice_; }

  /**
   * Return the kernel priority value, as visible on the 'prio :' line in
   * `/proc/<pid>/sched` file.
   *
   * The lower value means the _higher_ priority.
   */
  unsigned int KernelPriority() const {
    switch (policy_) {
      case SchedPolicy::kIdle:
        // For SCHED_IDLE kernel priority is always 120
        return kKernelDefaultPrio;
      case SchedPolicy::kOther:
      case SchedPolicy::kBatch:
        return static_cast<unsigned int>(kKernelDefaultPrio + nice_);
      case SchedPolicy::kFifo:
      case SchedPolicy::kRr:
        return kKernelMaxRtPrio - 1 - rt_priority_;
    }
    PERFETTO_FATAL("Can't be here (Unknown sched policy enum value %d)",
                   static_cast<int>(policy_));
  }

  unsigned int KernelPolicy() const;

  static Status ValidateNiceValue(const int nice) {
    if (nice >= kMinNice && nice <= kMaxNice) {
      return OkStatus();
    }
    return ErrStatus("Invalid nice value: %d. Valid range is [%d, %d]", nice,
                     kMinNice, kMaxNice);
  }

  static Status ValidatePriority(const unsigned int priority) {
    if (priority >= kMinPriority && priority <= kMaxPriority) {
      return OkStatus();
    }
    return ErrStatus("Invalid priority: %d. Valid range is [%d, %d]", priority,
                     kMinPriority, kMaxPriority);
  }

 private:
  unsigned int InternalUnifiedPriority() const {
    if (policy_ == SchedPolicy::kIdle) {
      return kKernelMaxPrio + 1;
    }
    return KernelPriority();
  }

  SchedPolicy policy_;
  unsigned int rt_priority_;
  int nice_;

  constexpr static int kMinNice = -20;     // inclusive
  constexpr static int kMaxNice = 19;      // inclusive
  constexpr static int kMinPriority = 1;   // inclusive
  constexpr static int kMaxPriority = 99;  // inclusive

  // defined as 'MAX_RT_PRIO' in 'linux/sched/prio.h'
  constexpr static int kKernelMaxRtPrio = 100;
  // defined as 'NICE_WIDTH' in 'linux/sched/prio.h'
  static constexpr int kNiceWidth = kMaxNice - kMinNice + 1;
  // defined as 'DEFAULT_PRIO' in 'linux/sched/prio.h'
  constexpr static int kKernelDefaultPrio = kKernelMaxRtPrio + kNiceWidth / 2;
  // defined as 'MAX_PRIO' in 'linux/sched/prio.h'
  constexpr static int kKernelMaxPrio = kKernelMaxRtPrio + kNiceWidth;
};

class SchedManagerInterface {
 public:
  virtual ~SchedManagerInterface();
  virtual bool IsSupportedOnTheCurrentPlatform() const = 0;
  virtual bool HasCapabilityToSetSchedPolicy() const = 0;
  virtual Status SetSchedConfig(const SchedConfig& arg) = 0;
  virtual StatusOr<SchedConfig> GetCurrentSchedConfig() const = 0;
};

class SchedManager final : public SchedManagerInterface {
  // Make it a friend to allow to invoke private constructor when creating a
  // static instance.
  friend class NoDestructor<SchedManager>;

  SchedManager() = default;

 public:
  static SchedManager& GetInstance() {
    static NoDestructor<SchedManager> instance;
    return instance.ref();
  }
  SchedManager(const SchedManager&) = delete;
  SchedManager& operator=(const SchedManager&) = delete;
  SchedManager(SchedManager&&) = delete;
  SchedManager& operator=(SchedManager&&) = delete;

  bool IsSupportedOnTheCurrentPlatform() const override;
  bool HasCapabilityToSetSchedPolicy() const override;
  Status SetSchedConfig(const SchedConfig& arg) override;
  StatusOr<SchedConfig> GetCurrentSchedConfig() const override;
};
}  // namespace perfetto::base

#endif  // INCLUDE_PERFETTO_EXT_BASE_SCHED_H_
