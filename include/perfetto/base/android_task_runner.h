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

#ifndef INCLUDE_PERFETTO_BASE_ANDROID_TASK_RUNNER_H_
#define INCLUDE_PERFETTO_BASE_ANDROID_TASK_RUNNER_H_

#include "perfetto/base/scoped_file.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/thread_checker.h"

#include <poll.h>
#include <chrono>
#include <map>
#include <mutex>
#include <queue>

#include <android/looper.h>

namespace perfetto {
namespace base {

// Runs a task runner on a thread owned by an Android Looper (ALooper).
class AndroidTaskRunner : public TaskRunner {
 public:
  AndroidTaskRunner();
  ~AndroidTaskRunner() override;

  // The following methods are only used in cases where the caller wants to take
  // ownership of the current thread (e.g., tests and standalone tools).
  // Normally the Android Framework runs the event loop on the thread. Run() can
  // only be called from the main thread but Quit() can be called from any
  // thread.
  void Run();
  void Quit();

  // Checks whether there are any pending immediate tasks to run. Note that
  // delayed tasks don't count even if they are due to run. Can only be called
  // from the main thread.
  bool IsIdleForTesting();

  // TaskRunner implementation:
  void PostTask(std::function<void()>) override;
  void PostDelayedTask(std::function<void()>, int delay_ms) override;
  void AddFileDescriptorWatch(int fd, std::function<void()>) override;
  void RemoveFileDescriptorWatch(int fd) override;

 private:
  class TimePoint : public timespec {
   public:
    TimePoint() { tv_sec = tv_nsec = 0; }

    bool operator<(const TimePoint& other) const {
      if (tv_sec == other.tv_sec)
        return tv_nsec < other.tv_nsec;
      return tv_sec < other.tv_sec;
    }

    explicit operator bool() const { return tv_sec || tv_nsec; }

    TimePoint AdvanceByMs(time_t ms) {
      const time_t kSecondsPerNs = 1000000000;
      PERFETTO_DCHECK(tv_nsec < kSecondsPerNs);
      TimePoint result(*this);
      time_t seconds = ms / 1000;
      result.tv_sec += seconds;
      ms -= seconds * 1000;
      result.tv_nsec += ms * 1000 * 1000;
      if (result.tv_nsec >= kSecondsPerNs) {
        result.tv_sec++;
        result.tv_nsec -= kSecondsPerNs;
      }
      return result;
    }
  };
  TimePoint GetTime() const;

  bool OnFileDescriptorEvent(int signalled_fd, int events);
  void RunImmediateTask();
  void RunDelayedTask();

  void GetNextDelayedTaskRunTimeLocked(struct itimerspec* runtime);

  void ScheduleImmediateWakeUp();
  void ScheduleDelayedWakeUp(const TimePoint& time);

  ALooper* const looper_;
  ScopedFile immediate_event_;
  ScopedFile delayed_timer_;

  ThreadChecker thread_checker_;

  // --- Begin lock-protected members.
  std::mutex lock_;
  // Note: std::deque allocates blocks of 4k in some implementations. Consider
  // another data structure if we end up having many task runner instances.
  std::deque<std::function<void()>> immediate_tasks_;
  std::multimap<TimePoint, std::function<void()>> delayed_tasks_;
  std::map<int, std::function<void()>> watch_tasks_;
  bool quit_ = false;
  // --- End lock-protected members.
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_ANDROID_TASK_RUNNER_H_
