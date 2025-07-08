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

#ifndef INCLUDE_PERFETTO_EXT_BASE_SCOPED_SCHED_BOOST_H_
#define INCLUDE_PERFETTO_EXT_BASE_SCOPED_SCHED_BOOST_H_

#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/thread_checker.h"

namespace perfetto::base {

// kSchedOther: it's the default policy (e.g. CFS on Linux). Range: 0-20.
//              prio is interpreted as -(nice), i.e. 1 is silhgly higher prio
//              than the default 0,  20 is the highest priority.
//              Note that this is the opposite semantic of the cmdline nice, and
//              is done for consistency with kSchedFifo, so higher
//              number == higher prio.
// kSchedFifo: real-time priority. Range: 1-99. 1 is the lowest priority, 99 the
//             highest.
struct SchedPolicyAndPrio {
  enum class Policy {
    kSchedOther,
    kSchedFifo
  };  // Order matters for operator<().

  bool operator<(const SchedPolicyAndPrio& other) const {
    return std::tie(policy, prio) < std::tie(other.policy, other.prio);
  }

  bool operator==(const SchedPolicyAndPrio& other) const {
    return policy == other.policy && prio == other.prio;
  }

  bool operator!=(const SchedPolicyAndPrio& other) const {
    return !(*this == other);
  }

  Policy policy = Policy::kSchedOther;
  unsigned int prio = 0;
};

class SchedOsManager {
 public:
  struct SchedOsConfig {
    int policy;
    int rt_prio;
    int nice;
  };

  static SchedOsManager* GetInstance();
  // virtual for testing
  virtual Status SetSchedConfig(const SchedOsConfig& arg);
  // virtual for testing
  virtual StatusOr<SchedOsConfig> GetCurrentSchedConfig() const;
  virtual ~SchedOsManager() = default;

 protected:
  SchedOsManager() = default;

  SchedOsManager(const SchedOsManager&) = delete;
  SchedOsManager& operator=(const SchedOsManager&) = delete;
  SchedOsManager(SchedOsManager&&) = delete;
  SchedOsManager& operator=(SchedOsManager&&) = delete;
};

// RAII helper to temporarily boost the scheduler priority of the current
// thread. The priority is reverted to the original value when the object goes
// out of scope.
// It is supported only on Linux/Android, fails on other platforms.
class ScopedSchedBoost {
 public:
  static StatusOr<ScopedSchedBoost> Boost(SchedPolicyAndPrio);
  ScopedSchedBoost(ScopedSchedBoost&&) noexcept;
  ScopedSchedBoost& operator=(ScopedSchedBoost&&) noexcept;
  ~ScopedSchedBoost();

  // No copy constructors.
  ScopedSchedBoost(const ScopedSchedBoost&) = delete;
  ScopedSchedBoost& operator=(const ScopedSchedBoost&) = delete;

  static void ResetForTesting(SchedOsManager*);

 private:
  explicit ScopedSchedBoost(SchedPolicyAndPrio p) : policy_and_prio_(p) {}

  std::optional<SchedPolicyAndPrio> policy_and_prio_;
  ThreadChecker thread_checker_;
};
}  // namespace perfetto::base

#endif  // INCLUDE_PERFETTO_EXT_BASE_SCOPED_SCHED_BOOST_H_
