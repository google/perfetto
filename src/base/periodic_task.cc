/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "perfetto/ext/base/periodic_task.h"

#include <limits>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    (PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && __ANDROID_API__ >= 19)
#include <sys/timerfd.h>
#endif

namespace perfetto {
namespace base {

namespace {
base::ScopedPlatformHandle CreateTimerFd(uint32_t period_ms) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    (PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && __ANDROID_API__ >= 19)
  base::ScopedPlatformHandle tfd(
      timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK));
  // The initial phase, aligned on wall clock.
  uint32_t phase_ms =
      period_ms -
      static_cast<uint32_t>(base::GetBootTimeNs().count() % period_ms);
  struct itimerspec its {};
  // The "1 +" is to make sure that we never pass a zero it_value in the
  // unlikely case of phase_ms being 0. That would cause the timer to be
  // considered disarmed by timerfd_settime.
  its.it_value.tv_sec = static_cast<time_t>(phase_ms / 1000u);
  its.it_value.tv_nsec = 1 + static_cast<long>((phase_ms % 1000u) * 1000000u);
  its.it_interval.tv_sec = static_cast<time_t>(period_ms / 1000u);
  its.it_interval.tv_nsec = static_cast<long>((period_ms % 1000u) * 1000000u);
  if (timerfd_settime(*tfd, 0, &its, nullptr) < 0)
    return base::ScopedPlatformHandle();
  return tfd;
#else
  base::ignore_result(period_ms);
  return base::ScopedPlatformHandle();
#endif
}
}  // namespace

PeriodicTask::PeriodicTask(base::TaskRunner* task_runner)
    : task_runner_(task_runner), weak_ptr_factory_(this) {}

PeriodicTask::~PeriodicTask() {
  Reset();
}

void PeriodicTask::Start(Args args) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  Reset();
  if (args.period_ms == 0 || !args.task) {
    PERFETTO_DCHECK(args.period_ms > 0);
    PERFETTO_DCHECK(args.task);
    return;
  }
  args_ = std::move(args);
  if (args_.use_suspend_aware_timer) {
    timer_fd_ = CreateTimerFd(args_.period_ms);
    if (timer_fd_) {
      auto weak_this = weak_ptr_factory_.GetWeakPtr();
      task_runner_->AddFileDescriptorWatch(
          *timer_fd_,
          std::bind(PeriodicTask::RunTaskAndPostNext, weak_this, generation_));
    } else {
      PERFETTO_DPLOG("timerfd not supported, falling back on PostDelayedTask");
    }
  }  // if (use_suspend_aware_timer).

  if (!timer_fd_)
    PostNextTask();

  if (args_.start_first_task_immediately)
    args_.task();
}

void PeriodicTask::PostNextTask() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DCHECK(args_.period_ms > 0);
  PERFETTO_DCHECK(!timer_fd_);
  uint32_t delay_ms =
      args_.period_ms -
      static_cast<uint32_t>(base::GetWallTimeMs().count() % args_.period_ms);
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      std::bind(PeriodicTask::RunTaskAndPostNext, weak_this, generation_),
      delay_ms);
}

// static
// This function can be called in two ways (both from the TaskRunner):
// 1. When using a timerfd, this task is registered as a FD watch.
// 2. When using PostDelayedTask, this is the task posted on the TaskRunner.
void PeriodicTask::RunTaskAndPostNext(base::WeakPtr<PeriodicTask> thiz,
                                      uint32_t generation) {
  if (!thiz || !thiz->args_.task || generation != thiz->generation_)
    return;  // Destroyed or Reset() in the meanwhile.
  PERFETTO_DCHECK_THREAD(thiz->thread_checker_);
  if (thiz->timer_fd_) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    PERFETTO_FATAL("timerfd for periodic tasks unsupported on Windows");
#else
    // If we are using a timerfd there is no need to repeatedly call
    // PostDelayedTask(). The kernel will wakeup the timer fd periodically. We
    // just need to read() it.
    uint64_t ignored = 0;
    errno = 0;
    auto rsize = base::Read(*thiz->timer_fd_, &ignored, sizeof(&ignored));
    if (rsize != sizeof(uint64_t)) {
      if (errno == EAGAIN)
        return;  // A spurious wakeup. Rare, but can happen, just ignore.
      PERFETTO_PLOG("read(timerfd) failed, falling back on PostDelayedTask");
      thiz->ResetTimerFd();
    }
#endif
  }
  // The repetition of the if() is to deal with the ResetTimerFd() case above.
  if (!thiz->timer_fd_) {
    thiz->PostNextTask();
  }
  // Create a copy of the task in the unlikely event that the task ends up
  // up destroying the PeriodicTask object or calling Reset() on it. That would
  // cause a reset of the args_.task itself, which would invalidate the task
  // bind state while we are invoking it.
  auto task = thiz->args_.task;
  task();
}

void PeriodicTask::Reset() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  ++generation_;
  args_ = Args();
  PERFETTO_DCHECK(!args_.task);
  ResetTimerFd();
}

void PeriodicTask::ResetTimerFd() {
  if (!timer_fd_)
    return;
  task_runner_->RemoveFileDescriptorWatch(*timer_fd_);
  timer_fd_.reset();
}

}  // namespace base
}  // namespace perfetto
