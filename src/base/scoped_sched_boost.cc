/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "perfetto/ext/base/scoped_sched_boost.h"

#include <algorithm>
#include <vector>

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <sys/resource.h>
#endif

#include "perfetto/base/thread_utils.h"

namespace perfetto {
namespace base {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
namespace {

// One instance per thread.
class ThreadMgr {
 public:
  static ThreadMgr& Instance() {
    static ThreadMgr* instance = new ThreadMgr();
    return *instance;
  }

  ThreadMgr();
  Status Add(SchedPolicyAndPrio);
  void Remove(SchedPolicyAndPrio);
  Status RecalcAndUpdatePrio();

  int initial_policy = SCHED_OTHER;
  sched_param initial_param{};
  int initial_nice = 0;
  std::vector<SchedPolicyAndPrio> prios_;
};

ThreadMgr::ThreadMgr() {
  initial_policy = sched_getscheduler(0);
  initial_param = {};
  sched_getparam(0, &initial_param);
  initial_nice = getpriority(PRIO_PROCESS, static_cast<id_t>(GetThreadId()));
}

Status ThreadMgr::Add(SchedPolicyAndPrio spp) {
  auto it = std::lower_bound(prios_.begin(), prios_.end(), spp);
  it = prios_.emplace(it, spp);
  bool is_highest = std::distance(it, prios_.end()) == 1;
  if (is_highest) {
    Status res = RecalcAndUpdatePrio();
    if (!res.ok()) {
      prios_.erase(it);
      return res;
    }
  }
  return OkStatus();
}

void ThreadMgr::Remove(SchedPolicyAndPrio spp) {
  auto it = std::find(prios_.begin(), prios_.end(), spp);
  if (it == prios_.end()) {
    return;  // Can happen if the creation failed, e.g. due to permission err.
  }
  bool recalc = std::distance(it, prios_.end()) == 1;
  prios_.erase(it);
  if (recalc)
    RecalcAndUpdatePrio();
}

Status ThreadMgr::RecalcAndUpdatePrio() {
  if (prios_.empty()) {
    if (sched_setscheduler(0, initial_policy, &initial_param)) {
      return ErrStatus("sched_setscheduler(initial) failed (%d)", errno);
    }
    if (initial_policy == SCHED_OTHER &&
        setpriority(PRIO_PROCESS, static_cast<id_t>(GetThreadId()),
                    initial_nice)) {
      return ErrStatus("setpriority(initial_nice) failed (%d)", errno);
    }
    return OkStatus();
  }

  SchedPolicyAndPrio& max_pol = prios_.back();
  sched_param param{};
  switch (max_pol.policy) {
    case SchedPolicyAndPrio::kInvalid:
      // We should never end up with a std::move(d) object in here.
      // The break will cause the CHECK(False) at the end to trap.
      break;
    case SchedPolicyAndPrio::kSchedFifo:
      param.sched_priority = max_pol.prio;
      if (sched_setscheduler(0, SCHED_FIFO, &param)) {
        return ErrStatus("sched_setscheduler(SCHED_FIFO, %d) failed (%d)",
                         param.sched_priority, errno);
      }
      return OkStatus();
    case SchedPolicyAndPrio::kSchedOther:
      if (sched_setscheduler(0, SCHED_OTHER, &param)) {
        return ErrStatus("sched_setscheduler(SCHED_OTHER, 0) failed (%d)",
                         errno);
      }

      int nice_level = -max_pol.prio;
      id_t cur_tid = static_cast<id_t>(GetThreadId());
      if (setpriority(PRIO_PROCESS, cur_tid, nice_level)) {
        return ErrStatus("setpriority(PRIO_PROCESS, %d, %d) failed (%d)",
                         static_cast<int>(cur_tid), nice_level, errno);
      }
      return OkStatus();
  }
  PERFETTO_CHECK(false);  // For GCC.
}

}  // namespace

// static
void ScopedSchedBoost::ResetForTesting() {
  ThreadMgr::Instance() = ThreadMgr();
}

// static
StatusOr<ScopedSchedBoost> ScopedSchedBoost::Boost(SchedPolicyAndPrio spp) {
  auto res = ThreadMgr::Instance().Add(spp);
  if (!res.ok())
    return res;
  return StatusOr<ScopedSchedBoost>(ScopedSchedBoost(spp));
}

ScopedSchedBoost::ScopedSchedBoost(ScopedSchedBoost&& other) noexcept {
  this->policy_and_prio_ = other.policy_and_prio_;
  other.policy_and_prio_.policy = SchedPolicyAndPrio::kInvalid;
}

ScopedSchedBoost& ScopedSchedBoost::operator=(
    ScopedSchedBoost&& other) noexcept {
  this->~ScopedSchedBoost();
  new (this) ScopedSchedBoost(std::move(other));
  return *this;
}

ScopedSchedBoost::~ScopedSchedBoost() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (policy_and_prio_.policy == SchedPolicyAndPrio::kInvalid)
    return;  // A std::move(d) object.
  ThreadMgr::Instance().Remove(policy_and_prio_);
  policy_and_prio_.policy = SchedPolicyAndPrio::kInvalid;
}

#else

// static
StatusOr<ScopedSchedBoost> ScopedSchedBoost::Boost(SchedPolicyAndPrio) {
  return ErrStatus("ScopedSchedBoost is supported only on Linux/Android");
}

ScopedSchedBoost::ScopedSchedBoost(ScopedSchedBoost&&) noexcept = default;
ScopedSchedBoost& ScopedSchedBoost::operator=(ScopedSchedBoost&&) noexcept =
    default;
ScopedSchedBoost::~ScopedSchedBoost() = default;

#endif

}  // namespace base
}  // namespace perfetto
