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

#include "perfetto/base/android_task_runner.h"

#include <errno.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

namespace perfetto {
namespace base {

AndroidTaskRunner::AndroidTaskRunner()
    : looper_(ALooper_prepare(0 /* require callbacks */)),
      immediate_event_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      delayed_timer_(
          timerfd_create(kWallTimeClockSource, TFD_NONBLOCK | TFD_CLOEXEC)) {
  ALooper_acquire(looper_);
  PERFETTO_CHECK(immediate_event_);
  PERFETTO_CHECK(delayed_timer_);
  AddFileDescriptorWatch(immediate_event_.get(),
                         std::bind(&AndroidTaskRunner::RunImmediateTask, this));
  AddFileDescriptorWatch(delayed_timer_.get(),
                         std::bind(&AndroidTaskRunner::RunDelayedTask, this));
}

AndroidTaskRunner::~AndroidTaskRunner() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  std::lock_guard<std::mutex> lock(lock_);
  for (const auto& watch : watch_tasks_) {
    // ALooper doesn't guarantee that each watch doesn't run one last time if
    // the file descriptor was already signalled. To guard against this point
    // the watch to a no-op callback.
    ALooper_addFd(
        looper_, watch.first, ALOOPER_POLL_CALLBACK,
        ALOOPER_EVENT_INPUT | ALOOPER_EVENT_ERROR | ALOOPER_EVENT_HANGUP,
        [](int, int, void*) -> int { return 0; }, nullptr);
    ALooper_removeFd(looper_, watch.first);
  }
  ALooper_release(looper_);

  struct itimerspec time = {};
  timerfd_settime(delayed_timer_.get(), TFD_TIMER_ABSTIME, &time, nullptr);
}

void AndroidTaskRunner::Run() {
  quit_ = false;
  while (true) {
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (quit_)
        break;
    }
    ALooper_pollOnce(-1 /* timeout */, nullptr, nullptr, nullptr);
  }
}

void AndroidTaskRunner::Quit() {
  std::lock_guard<std::mutex> lock(lock_);
  quit_ = true;
  ALooper_wake(looper_);
}

bool AndroidTaskRunner::IsIdleForTesting() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  std::lock_guard<std::mutex> lock(lock_);
  return immediate_tasks_.empty();
}

void AndroidTaskRunner::RunImmediateTask() {
  uint64_t unused = 0;
  if (read(immediate_event_.get(), &unused, sizeof(unused)) != sizeof(unused) &&
      errno != EAGAIN) {
    PERFETTO_DPLOG("read");
  }

  // If locking overhead becomes an issue, add a separate work queue.
  bool has_next;
  std::function<void()> immediate_task;
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (immediate_tasks_.empty())
      return;
    immediate_task = std::move(immediate_tasks_.front());
    immediate_tasks_.pop_front();
    has_next = !immediate_tasks_.empty();
  }
  // Do another pass through the event loop even if we have immediate tasks to
  // run for fairness.
  if (has_next)
    ScheduleImmediateWakeUp();
  errno = 0;
  RunTask(immediate_task);
}

void AndroidTaskRunner::RunDelayedTask() {
  uint64_t unused = 0;
  if (read(delayed_timer_.get(), &unused, sizeof(unused)) != sizeof(unused) &&
      errno != EAGAIN) {
    PERFETTO_DPLOG("read");
  }

  std::function<void()> delayed_task;
  TimeMillis next_wake_up{};
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (delayed_tasks_.empty())
      return;
    auto it = delayed_tasks_.begin();
    PERFETTO_DCHECK(!(GetWallTimeMs() < it->first));
    delayed_task = std::move(it->second);
    delayed_tasks_.erase(it);
    if (!delayed_tasks_.empty())
      next_wake_up = delayed_tasks_.begin()->first;
  }
  if (next_wake_up.count())
    ScheduleDelayedWakeUp(next_wake_up);
  errno = 0;
  RunTask(delayed_task);
}

void AndroidTaskRunner::ScheduleImmediateWakeUp() {
  uint64_t value = 1;
  if (write(immediate_event_.get(), &value, sizeof(value)) == -1 &&
      errno != EAGAIN) {
    PERFETTO_DPLOG("write");
  }
}

void AndroidTaskRunner::ScheduleDelayedWakeUp(TimeMillis time) {
  PERFETTO_DCHECK(time.count());
  struct itimerspec wake_up = {};
  wake_up.it_value = ToPosixTimespec(time);
  if (timerfd_settime(delayed_timer_.get(), TFD_TIMER_ABSTIME, &wake_up,
                      nullptr) == -1) {
    PERFETTO_DPLOG("timerfd_settime");
  }
}

void AndroidTaskRunner::PostTask(std::function<void()> task) {
  bool was_empty;
  {
    std::lock_guard<std::mutex> lock(lock_);
    was_empty = immediate_tasks_.empty();
    immediate_tasks_.push_back(std::move(task));
  }
  if (was_empty)
    ScheduleImmediateWakeUp();
}

void AndroidTaskRunner::PostDelayedTask(std::function<void()> task,
                                        uint32_t delay_ms) {
  PERFETTO_DCHECK(delay_ms >= 0);
  TimeMillis runtime = GetWallTimeMs() + TimeMillis(delay_ms);
  bool is_next = false;
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto it = delayed_tasks_.insert(std::make_pair(runtime, std::move(task)));
    if (it == delayed_tasks_.begin())
      is_next = true;
  }
  if (is_next)
    ScheduleDelayedWakeUp(runtime);
}

void AndroidTaskRunner::AddFileDescriptorWatch(int fd,
                                               std::function<void()> task) {
  PERFETTO_DCHECK(fd >= 0);
  {
    std::lock_guard<std::mutex> lock(lock_);
    PERFETTO_DCHECK(!watch_tasks_.count(fd));
    watch_tasks_[fd] = std::move(task);
  }
  // It's safe for the callback to hang on to |this| as everything is
  // unregistered in the destructor.
  auto callback = [](int signalled_fd, int events, void* data) -> int {
    AndroidTaskRunner* task_runner = reinterpret_cast<AndroidTaskRunner*>(data);
    return task_runner->OnFileDescriptorEvent(signalled_fd, events) ? 1 : 0;
  };
  PERFETTO_CHECK(ALooper_addFd(looper_, fd, ALOOPER_POLL_CALLBACK,
                               ALOOPER_EVENT_INPUT | ALOOPER_EVENT_ERROR |
                                   ALOOPER_EVENT_HANGUP,
                               std::move(callback), this) != -1);
}

bool AndroidTaskRunner::OnFileDescriptorEvent(int signalled_fd, int events) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!(events & (ALOOPER_EVENT_INPUT | ALOOPER_EVENT_ERROR |
                  ALOOPER_EVENT_HANGUP | ALOOPER_EVENT_INVALID))) {
    return true;
  }
  std::function<void()> task;
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto it = watch_tasks_.find(signalled_fd);
    if (it == watch_tasks_.end())
      return false;
    task = it->second;
  }
  errno = 0;
  RunTask(task);
  return true;
}

void AndroidTaskRunner::RemoveFileDescriptorWatch(int fd) {
  PERFETTO_DCHECK(fd >= 0);
  {
    std::lock_guard<std::mutex> lock(lock_);
    PERFETTO_DCHECK(watch_tasks_.count(fd));
    watch_tasks_.erase(fd);
  }
  ALooper_removeFd(looper_, fd);
}

}  // namespace base
}  // namespace perfetto
