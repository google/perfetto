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

#include "perfetto/base/unix_task_runner.h"

#include "perfetto/base/build_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

namespace perfetto {
namespace base {

UnixTaskRunner::UnixTaskRunner() {
  // Create a self-pipe which is used to wake up the main thread from inside
  // poll(2).
  int pipe_fds[2];
  PERFETTO_CHECK(pipe(pipe_fds) == 0);

  // Make the pipe non-blocking so that we never block the waking thread (either
  // the main thread or another one) when scheduling a wake-up.
  for (auto fd : pipe_fds) {
    int flags = fcntl(fd, F_GETFL, 0);
    PERFETTO_CHECK(flags != -1);
    PERFETTO_CHECK(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    PERFETTO_CHECK(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
  }
  control_read_.reset(pipe_fds[0]);
  control_write_.reset(pipe_fds[1]);

#if BUILDFLAG(OS_LINUX)
  // We are never expecting to have more than a few bytes in the wake-up pipe.
  // Reduce the buffer size on Linux. Note that this gets rounded up to the page
  // size.
  PERFETTO_CHECK(fcntl(control_read_.get(), F_SETPIPE_SZ, 1) > 0);
#endif

  AddFileDescriptorWatch(control_read_.get(), [] {
    // Not reached -- see PostFileDescriptorWatches().
    PERFETTO_DCHECK(false);
  });
}

UnixTaskRunner::~UnixTaskRunner() = default;

UnixTaskRunner::TimePoint UnixTaskRunner::GetTime() const {
  return std::chrono::steady_clock::now();
}

void UnixTaskRunner::WakeUp() {
  const char dummy = 'P';
  if (write(control_write_.get(), &dummy, 1) <= 0 && errno != EAGAIN)
    PERFETTO_DPLOG("write()");
}

void UnixTaskRunner::Run() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  quit_ = false;
  while (true) {
    int poll_timeout_ms;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (quit_)
        return;
      poll_timeout_ms = static_cast<int>(GetDelayToNextTaskLocked().count());
      UpdateWatchTasksLocked();
    }
    int ret = PERFETTO_EINTR(poll(
        &poll_fds_[0], static_cast<nfds_t>(poll_fds_.size()), poll_timeout_ms));
    PERFETTO_CHECK(ret >= 0);

    // To avoid starvation we always interleave all types of tasks -- immediate,
    // delayed and file descriptor watches.
    PostFileDescriptorWatches();
    RunImmediateAndDelayedTask();
  }
}

void UnixTaskRunner::Quit() {
  {
    std::lock_guard<std::mutex> lock(lock_);
    quit_ = true;
  }
  WakeUp();
}

bool UnixTaskRunner::IsIdleForTesting() {
  std::lock_guard<std::mutex> lock(lock_);
  return immediate_tasks_.empty();
}

void UnixTaskRunner::UpdateWatchTasksLocked() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!watch_tasks_changed_)
    return;
  watch_tasks_changed_ = false;
  poll_fds_.clear();
  for (auto& it : watch_tasks_) {
    it.second.poll_fd_index = poll_fds_.size();
    poll_fds_.push_back({it.first, POLLIN | POLLHUP, 0});
  }
}

void UnixTaskRunner::RunImmediateAndDelayedTask() {
  // TODO(skyostil): Add a separate work queue in case in case locking overhead
  // becomes an issue.
  std::function<void()> immediate_task;
  std::function<void()> delayed_task;
  auto now = GetTime();
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (!immediate_tasks_.empty()) {
      immediate_task = std::move(immediate_tasks_.front());
      immediate_tasks_.pop_front();
    }
    if (!delayed_tasks_.empty()) {
      auto it = delayed_tasks_.begin();
      if (now >= it->first) {
        delayed_task = std::move(it->second);
        delayed_tasks_.erase(it);
      }
    }
  }

  errno = 0;
  if (immediate_task)
    immediate_task();

  errno = 0;
  if (delayed_task)
    delayed_task();
}

void UnixTaskRunner::PostFileDescriptorWatches() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  for (size_t i = 0; i < poll_fds_.size(); i++) {
    if (!(poll_fds_[i].revents & (POLLIN | POLLHUP)))
      continue;
    poll_fds_[i].revents = 0;

    // The wake-up event is handled inline to avoid an infinite recursion of
    // posted tasks.
    if (poll_fds_[i].fd == control_read_.get()) {
      // Drain the byte(s) written to the wake-up pipe. We can potentially read
      // more than one byte if several wake-ups have been scheduled.
      char buffer[16];
      if (read(control_read_.get(), &buffer[0], sizeof(buffer)) <= 0 &&
          errno != EAGAIN) {
        PERFETTO_DPLOG("read()");
      }
      continue;
    }

    // Binding to |this| is safe since we are the only object executing the
    // task.
    PostTask(std::bind(&UnixTaskRunner::RunFileDescriptorWatch, this,
                       poll_fds_[i].fd));

    // Make the fd negative while a posted task is pending. This makes poll(2)
    // ignore the fd.
    PERFETTO_DCHECK(poll_fds_[i].fd >= 0);
    poll_fds_[i].fd = -poll_fds_[i].fd;
  }
}

void UnixTaskRunner::RunFileDescriptorWatch(int fd) {
  std::function<void()> task;
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto it = watch_tasks_.find(fd);
    if (it == watch_tasks_.end())
      return;
    // Make poll(2) pay attention to the fd again. Since another thread may have
    // updated this watch we need to refresh the set first.
    UpdateWatchTasksLocked();
    size_t fd_index = it->second.poll_fd_index;
    PERFETTO_DCHECK(fd_index < poll_fds_.size());
    PERFETTO_DCHECK(::abs(poll_fds_[fd_index].fd) == fd);
    poll_fds_[fd_index].fd = fd;
    task = it->second.callback;
  }
  errno = 0;
  task();
}

UnixTaskRunner::TimeDurationMs UnixTaskRunner::GetDelayToNextTaskLocked()
    const {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!immediate_tasks_.empty())
    return TimeDurationMs(0);
  if (!delayed_tasks_.empty()) {
    return std::max(TimeDurationMs(0),
                    std::chrono::duration_cast<TimeDurationMs>(
                        delayed_tasks_.begin()->first - GetTime()));
  }
  return TimeDurationMs(-1);
}

void UnixTaskRunner::PostTask(std::function<void()> task) {
  bool was_empty;
  {
    std::lock_guard<std::mutex> lock(lock_);
    was_empty = immediate_tasks_.empty();
    immediate_tasks_.push_back(std::move(task));
  }
  if (was_empty)
    WakeUp();
}

void UnixTaskRunner::PostDelayedTask(std::function<void()> task, int delay_ms) {
  PERFETTO_DCHECK(delay_ms >= 0);
  auto runtime = GetTime() + std::chrono::milliseconds(delay_ms);
  {
    std::lock_guard<std::mutex> lock(lock_);
    delayed_tasks_.insert(std::make_pair(runtime, std::move(task)));
  }
  WakeUp();
}

void UnixTaskRunner::AddFileDescriptorWatch(int fd,
                                            std::function<void()> task) {
  PERFETTO_DCHECK(fd >= 0);
  {
    std::lock_guard<std::mutex> lock(lock_);
    PERFETTO_DCHECK(!watch_tasks_.count(fd));
    watch_tasks_[fd] = {std::move(task), SIZE_MAX};
    watch_tasks_changed_ = true;
  }
  WakeUp();
}

void UnixTaskRunner::RemoveFileDescriptorWatch(int fd) {
  PERFETTO_DCHECK(fd >= 0);
  {
    std::lock_guard<std::mutex> lock(lock_);
    PERFETTO_DCHECK(watch_tasks_.count(fd));
    watch_tasks_.erase(fd);
    watch_tasks_changed_ = true;
  }
  // No need to schedule a wake-up for this.
}

}  // namespace base
}  // namespace perfetto
