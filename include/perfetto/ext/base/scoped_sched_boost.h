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

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/thread_checker.h"

namespace perfetto {
namespace base {

// kSchedOther: it's the default policy (e.g. CFS on Linux). Range: 0-20.
//              prio is interpreted as -(nice), i.e. 1 is silhgly higher prio
//              than the default 0,  20 is the highest priority.
//              Note that this is the opposite semantic of the cmdline nice, and
//              is done for consistency with kSchedFifo, so higher
//              number == higher prio.
// kSchedFifo: real-time priority. Range: 1-99. 1 is the lowest priority, 99 the
//             highest.
struct SchedPolicyAndPrio {
  enum Policy {
    kInvalid = 0,
    kSchedOther,
    kSchedFifo
  };  // Order matters for operator<().

  SchedPolicyAndPrio(Policy policy_, int prio_)
      : policy(policy_), prio(prio_) {}

  bool operator<(const SchedPolicyAndPrio& other) const {
    if (policy != other.policy) {
      return policy < other.policy;
    }
    return prio < other.prio;
  }

  bool operator==(const SchedPolicyAndPrio& other) const {
    return policy == other.policy && prio == other.prio;
  }

  bool operator!=(const SchedPolicyAndPrio& other) const {
    return !(*this == other);
  }

  Policy policy;
  int prio;
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

  // No copy (the dtor needs changes if we want to support move).
  ScopedSchedBoost(const ScopedSchedBoost&) = delete;
  ScopedSchedBoost& operator=(const ScopedSchedBoost&) = delete;

 private:
  explicit ScopedSchedBoost(SchedPolicyAndPrio p) : policy_and_prio_(p) {}

  SchedPolicyAndPrio policy_and_prio_{SchedPolicyAndPrio::kInvalid, 0};
  ThreadChecker thread_checker_;
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_SCOPED_SCHED_BOOST_H_
