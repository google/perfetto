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

#ifndef INCLUDE_PERFETTO_EXT_BASE_LOCK_FREE_TASK_RUNNER_H_
#define INCLUDE_PERFETTO_EXT_BASE_LOCK_FREE_TASK_RUNNER_H_

#include "perfetto/base/flat_set.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/thread_annotations.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/atomic_shared_ptr.h"
#include "perfetto/ext/base/event_fd.h"
#include "perfetto/ext/base/flags.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/unix_task_runner.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <poll.h>
#endif

#include <array>
#include <atomic>
#include <map>
#include <thread>

namespace perfetto {
namespace base {

// This class implements a lock-less multi-producer single-consumer task runner.
// This is achieved by using a linked list of "slabs". Each slab is a fixed-size
// array of tasks.
//
// The overall architecture is as follows:
// - There is one "main" thread, which is the only thread that is allowed to
//   invoke Run(). This is the consumer thread.
// - There can be multiple "writer" threads, which are the threads that call
//   PostTask(). These are the producer threads.
//
// The slabs are organized as a singly-linked list, linked from the tail:
// tail -> [Slab N] -> [Slab N-1] -> ... -> [Slab 0] -> null
// The tail points to the latest Slab. In nominal cases (i.e. in absence of
// PostTask() bursts, assuming Run catches up) there is only one (or at most
// two) Slabs in the list.
//
// Writer threads atomically try to reserve a slot in the current `tail` slab.
// If the slab is full, they allocate a new slab and atomically swap the
// `tail` pointer to point to the new slab, linking the old tail as `prev`.
//
// The key design element is that writer threads only ever access the `tail`
// slab and never look at the `->prev` pointer / never iterate the list.
// Only the main Run() thread iterates the list.  This makes the design simpler
// to reason about.
//
// The main thread, instead, is the only one that is allowed to follow the
// `->prev` pointers to drain the tasks.
//
// Slab lifecycle:
// - A new slab is created by a writer thread when the current slab is full.
// - The main thread drains tasks from slabs (from 0 to N). When a slab becomes
//   empty, it's destroyed using a shared_ptr, which guarrantees that the slab
//   is not destroyed while another writer thread is trying to append tasks.
// - As a further optimization, empty slabs are kept around in a free-list of
//   size 1. This is makes it so that in absence of bursts this class doesn't
//   perform any allocation.
//
//                    tail_ (atomic_shared_ptr)
//                        |
//                        ▼
//      +-----------------+      +-----------------+      +-----------------+
//      |     Slab N      |      |    Slab N-1     |      |     Slab 0      |
//      | tasks: [....]   |      | tasks: [....]   |      | tasks: [....]   |
//      | next_task_slot  |      | next_task_slot  |      | next_task_slot  |
//      | prev (sptr) ----+----->| prev (sptr) ----+----->| prev = nullptr  |
//      +-----------------+      +-----------------+      +-----------------+
//
class PERFETTO_EXPORT_COMPONENT LockFreeTaskRunner : public TaskRunner {
 public:
  LockFreeTaskRunner();
  ~LockFreeTaskRunner() override;

  void Run();
  void Quit();

  // Checks whether there are any pending immediate tasks to run. Note that
  // delayed tasks don't count even if they are due to run.
  bool IsIdleForTesting();

  // TaskRunner implementation:
  void PostTask(std::function<void()>) override;
  void PostDelayedTask(std::function<void()>, uint32_t delay_ms) override;
  void AddFileDescriptorWatch(PlatformHandle, std::function<void()>) override;
  void RemoveFileDescriptorWatch(PlatformHandle) override;
  bool RunsTasksOnCurrentThread() const override;

  // Pretends (for the purposes of running delayed tasks) that time advanced by
  // `ms`.
  void AdvanceTimeForTesting(uint32_t ms);

  static constexpr size_t kSlabSize = 512;  // Exposed for testing.

  // Stats for testing.
  size_t slabs_allocated() const {
    return slabs_allocated_.load(std::memory_order_relaxed);
  }
  size_t slabs_freed() const {
    return slabs_freed_.load(std::memory_order_relaxed);
  }

 private:
  // A slab is a fixed-size array of tasks. The lifecycle of a task slot
  // within a slab goes through three phases:
  //
  // 1. Reservation: A writer thread atomically increments `next_task_slot` to
  //    reserve a slot in the `tasks` array. This reservation establishes the
  //    implicit order in which the consumer will attempt to read tasks (but
  //    only if they are published in the bitmap, see below).
  //
  // 2. Publishing: After writing the task into its reserved slot, the writer
  //    thread atomically sets the corresponding bit in the `tasks_written`
  //    bitmask. This acts as a memory barrier and makes the task visible to
  //    the consumer (main) thread.
  //
  // 3. Consumption: The main thread acquire-reads the `tasks_written` bitmask.
  //    For each bit that is set, it processes the task and then sets the
  //    corresponding bit in its private `tasks_read` bitmask to prevent
  //    reading the same task again.
  struct Slab {
    Slab();
    ~Slab();

    // `tasks` and `next_task_slot` are accessed by writer threads only.
    // The main thread can access `tasks[i]` but only after ensuring that the
    // corresponding bit in `tasks_written` is set.
    std::array<std::function<void()>, kSlabSize> tasks{};
    std::atomic<size_t> next_task_slot{0};

    // A bitmask indicating which tasks in the `tasks` array have been written
    // and are ready to be read by the main thread.
    // This is atomically updated by writer threads and read by the main thread.
    using BitWord = size_t;
    static constexpr size_t kBitsPerWord = sizeof(BitWord) * 8;
    static constexpr size_t kNumWords = kSlabSize / kBitsPerWord;
    std::array<std::atomic<BitWord>, kNumWords> tasks_written{};

    // A bitmask indicating which tasks have been read by the main thread.
    // This is accessed only by the main thread, so no atomicity is required.
    std::array<BitWord, kNumWords> tasks_read{};

    // The link to the previous slab.
    // This is written by writer threads when they create a new slab and link it
    // to the previous tail. But they do so when nobody else can see the Slab,
    // so there is no need for an AtomicSharedPtr. After the initial creation,
    // this is accessed only by the main thread when:
    // 1. draining tasks (to walk back to the oldest slab)
    // 2. deleting slabs, setting it to nullptr, when they are fully consumed.
    std::shared_ptr<Slab> prev;
  };

  struct DelayedTask {
    TimeMillis time;
    uint64_t seq;
    std::function<void()> task;

    // Note that the < operator keeps the DelayedTasks sorted in reverse order
    // (the latest one is first, the earliest one is last). This is so we can
    // have a FIFO queue using a vector by just doing an O(1) pop_back().
    bool operator<(const DelayedTask& other) const {
      if (time != other.time)
        return time > other.time;
      return seq > other.seq;
    }
    bool operator==(const DelayedTask& other) const {
      return time == other.time && seq == other.seq;
    }
  };

  std::function<void()> PopNextImmediateTask();
  std::function<void()> PopTaskRecursive(const std::shared_ptr<Slab>&,
                                         Slab* next_slab);
  std::function<void()> PopNextExpiredDelayedTask();
  int GetDelayMsToNextTask() const;
  void WakeUp() { wakeup_event_.Notify(); }
  std::shared_ptr<Slab> AllocNewSlab();
  void PostFileDescriptorWatches(uint64_t windows_wait_result);
  void RunFileDescriptorWatch(PlatformHandle);
  void UpdateWatchTasks();

  // This is semantically a unique_ptr, but is accessed from different threads.
  std::atomic<Slab*> free_slab_{};

  EventFd wakeup_event_;
  bool quit_ = false;
  std::thread::id run_task_thread_id_;

  // Delayed tasks, accessed only by the main thread. Items are stored in
  // reverse temporal order, see comment in the operator<.
  FlatSet<DelayedTask> delayed_tasks_;
  uint64_t next_delayed_task_seq_ = 0;
  std::atomic<uint32_t> advanced_time_for_testing_{};

  // The array of fds/handles passed to poll(2) / WaitForMultipleObjects().
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  std::vector<PlatformHandle> poll_fds_;
#else
  std::vector<struct pollfd> poll_fds_;
#endif

  struct WatchTask {
    std::function<void()> callback;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    // On UNIX systems we make the FD number negative in |poll_fds_| to avoid
    // polling it again until the queued task runs. On Windows we can't do that.
    // Instead we keep track of its state here.
    bool pending = false;
#else
    size_t poll_fd_index;  // Index into |poll_fds_|.
#endif
  };

  // Accessed only from the main thread.
  std::map<PlatformHandle, WatchTask> watch_tasks_;
  bool watch_tasks_changed_ = false;

  // Stats for testing.
  std::atomic<size_t> slabs_allocated_{};
  std::atomic<size_t> slabs_freed_{};

  // Keep last, so deletion of slabs happens before invalidating the remaining
  // state.
  AtomicSharedPtr<Slab> tail_;
};

using MaybeLockFreeTaskRunner =
    std::conditional_t<base::flags::use_lockfree_taskrunner,
                       LockFreeTaskRunner,
                       UnixTaskRunner>;

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_LOCK_FREE_TASK_RUNNER_H_
