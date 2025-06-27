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
#include <pthread.h>
#include <sys/resource.h>
#endif

#include "perfetto/base/thread_utils.h"

namespace perfetto {
namespace base {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
namespace {

// One instance per thread.
class CurThread {
 public:
  CurThread() {
    PERFETTO_CHECK(pthread_getschedparam(pthread_self(), &initial_policy,
                                         &initial_param) == 0);
  }

  Status RecalcAndUpdatePrio();

  int initial_policy = SCHED_OTHER;
  sched_param initial_param{};
  std::vector<SchedPolicyAndPrio> prios_;
};

thread_local CurThread sched_mgr;

Status CurThread::RecalcAndUpdatePrio() {
  pthread_t self = pthread_self();
  if (prios_.empty()) {
    PERFETTO_CHECK(
        pthread_setschedparam(self, initial_policy, &initial_param) == 0);
  }
  SchedPolicyAndPrio& max_pol = prios_.front();
  sched_param param{};
  switch (max_pol.policy) {
    case SchedPolicyAndPrio::kInvalid:
      // We should never end up with a std::move(d) object in here.
      // The break will cause the CHECK(False) at the end to trap.
      break;
    case SchedPolicyAndPrio::kSchedFifo:
      param.sched_priority = max_pol.prio;
      if (pthread_setschedparam(self, SCHED_FIFO, &param)) {
        return ErrStatus(
            "pthread_setschedparam(SCHED_FIFO, %d) failed errno=%d",
            param.sched_priority, errno);
      }
      return OkStatus();
    case SchedPolicyAndPrio::kSchedOther:
      if (pthread_setschedparam(self, SCHED_OTHER, &param)) {
        return ErrStatus(
            "pthread_setschedparam(SCHED_OTHER, 0) failed, errno=%d", errno);
      }

      int nice_level = -max_pol.prio;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
      if (setpriority(PRIO_DARWIN_THREAD, 0, nice_level)) {
        return ErrStatus(
            "setpriority(PRIO_DARWIN_THREAD, 0, %d) failed, errno=%d",
            nice_level, errno);
      }
#else
      int cur_tid = static_cast<int>(GetThreadId());
      if (setpriority(PRIO_PROCESS, cur_tid, nice_level)) {
        return ErrStatus("setpriority(PRIO_PROCESS, %d, %d) failed, errno=%d",
                         cur_tid, nice_level, errno);
      }
#endif
      return OkStatus();
  }
  PERFETTO_CHECK(false);  // For GCC.
}

}  // namespace

// static
StatusOr<ScopedSchedBoost> ScopedSchedBoost::Boost(SchedPolicyAndPrio spp) {
  auto& prios = sched_mgr.prios_;
  auto it = std::lower_bound(prios.begin(), prios.end(), spp);
  bool boost = it == prios.begin();
  it = prios.emplace(it, spp);
  if (boost) {
    Status res = sched_mgr.RecalcAndUpdatePrio();
    if (!res.ok()) {
      prios.erase(it);
      return res;
    }
  }
  return StatusOr<ScopedSchedBoost>(ScopedSchedBoost(spp));
}

ScopedSchedBoost::ScopedSchedBoost(ScopedSchedBoost&& other) noexcept {
  *this = std::move(other);
}

ScopedSchedBoost& ScopedSchedBoost::operator=(
    ScopedSchedBoost&& other) noexcept {
  this->policy_and_prio_ = other.policy_and_prio_;
  other.policy_and_prio_.policy = SchedPolicyAndPrio::kInvalid;
  return *this;
}

ScopedSchedBoost::~ScopedSchedBoost() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (policy_and_prio_.policy == SchedPolicyAndPrio::kInvalid) {
    return;  // A std::move(d) object.
  }
  auto& prios = sched_mgr.prios_;
  auto it = std::find(prios.begin(), prios.end(), policy_and_prio_);
  if (it == prios.end()) {
    return;  // Can happen if the creation failed, e.g. due to permission err.
  }
  bool recalc = it == prios.begin();
  prios.erase(it);
  if (recalc)
    sched_mgr.RecalcAndUpdatePrio();
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
