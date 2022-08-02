/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/importers/ftrace/thread_state_tracker.h"

#include <algorithm>

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

constexpr uint32_t CPU_A = 0;
constexpr uint32_t CPU_B = 1;
constexpr UniqueTid IDLE_THREAD = 0;
constexpr UniqueTid THREAD_A = 1;
constexpr UniqueTid THREAD_B = 2;
constexpr UniqueTid THREAD_C = 3;
static constexpr char kRunning[] = "Running";
static constexpr char kRunnable[] = "R";
static constexpr char kBlockedFunction[] = "blocked1";

class ThreadStateTrackerUnittest : public testing::Test {
 public:
  ThreadStateTrackerUnittest() {
    context_.storage.reset(new TraceStorage());
    context_.global_args_tracker.reset(
        new GlobalArgsTracker(context_.storage.get()));
    context_.args_tracker.reset(new ArgsTracker(&context_));
    tracker_.reset(new ThreadStateTracker(context_.storage.get()));
  }

  StringId StringIdOf(const char* s) {
    return context_.storage->InternString(s);
  }

  tables::ThreadStateTable::ConstIterator ThreadStateIterator() {
    return context_.storage->thread_state_table().FilterToIterator({});
  }

  void VerifyThreadState(
      const tables::ThreadStateTable::ConstIterator& it,
      int64_t from,
      base::Optional<int64_t> to,
      UniqueTid utid,
      const char* state,
      base::Optional<bool> io_wait = base::nullopt,
      base::Optional<StringId> blocked_function = base::nullopt,
      base::Optional<UniqueTid> waker_utid = base::nullopt,
      base::Optional<int64_t> cpu = base::nullopt) {
    ASSERT_EQ(it.ts(), from);
    ASSERT_EQ(it.dur(), to ? *to - from : -1);
    ASSERT_EQ(it.utid(), utid);
    if (state == kRunning) {
      if (cpu.has_value()) {
        ASSERT_EQ(it.cpu(), cpu);
      } else {
        ASSERT_EQ(it.cpu(), CPU_A);
      }
    } else {
      ASSERT_EQ(it.cpu(), base::nullopt);
    }
    ASSERT_STREQ(context_.storage->GetString(it.state()).c_str(), state);
    ASSERT_EQ(it.io_wait(), io_wait);
    ASSERT_EQ(it.blocked_function(), blocked_function);
    ASSERT_EQ(it.waker_utid(), waker_utid);
  }

 protected:
  std::unique_ptr<ThreadStateTracker> tracker_;
  TraceProcessorContext context_;
  StringId running_string_id_;
  StringId runnable_string_id_;
  StringId sched_blocked_reason_id_;
};

TEST_F(ThreadStateTrackerUnittest, BasicPushSchedSwitchEvent) {
  tracker_->PushSchedSwitchEvent(10, CPU_A, THREAD_A, StringIdOf("S"),
                                 THREAD_B);

  ASSERT_EQ(context_.storage->thread_state_table().row_count(), 2ul);
  auto rows_it = ThreadStateIterator();
  VerifyThreadState(rows_it, 10, base::nullopt, THREAD_A, "S");
  VerifyThreadState(++rows_it, 10, base::nullopt, THREAD_B, kRunning);
}

TEST_F(ThreadStateTrackerUnittest, StartWithWakingEvent) {
  tracker_->PushWakingEvent(10, THREAD_A, THREAD_C);
  ASSERT_EQ(context_.storage->thread_state_table().row_count(), 0ul);
}

TEST_F(ThreadStateTrackerUnittest, BasicWakingEvent) {
  tracker_->PushSchedSwitchEvent(10, CPU_A, THREAD_A, StringIdOf("S"),
                                 THREAD_B);
  tracker_->PushWakingEvent(20, THREAD_A, THREAD_C);

  ASSERT_EQ(context_.storage->thread_state_table().row_count(), 3ul);
  auto row_it = ThreadStateIterator();
  VerifyThreadState(row_it, 10, 20, THREAD_A, "S");
  VerifyThreadState(++row_it, 10, base::nullopt, THREAD_B, kRunning);
  VerifyThreadState(++row_it, 20, base::nullopt, THREAD_A, kRunnable,
                    base::nullopt, base::nullopt, THREAD_C);
}

TEST_F(ThreadStateTrackerUnittest, BasicPushBlockedReason) {
  tracker_->PushSchedSwitchEvent(10, CPU_A, THREAD_A, StringIdOf("S"),
                                 THREAD_B);
  tracker_->PushBlockedReason(THREAD_A, true, StringIdOf(kBlockedFunction));

  auto rows_it = ThreadStateIterator();
  VerifyThreadState(rows_it, 10, base::nullopt, THREAD_A, "S", true,
                    StringIdOf(kBlockedFunction));
}

TEST_F(ThreadStateTrackerUnittest, CloseState) {
  // Add a new runnable state of THREAD_A at ts=10.
  tracker_->PushSchedSwitchEvent(10, CPU_A, THREAD_A, StringIdOf(kRunnable),
                                 THREAD_B);

  // Close the runnable state of THREAD_A at ts=20 and make it run on the CPU.
  tracker_->PushSchedSwitchEvent(20, CPU_A, THREAD_B, StringIdOf("S"),
                                 THREAD_A);

  auto rows_it = ThreadStateIterator();
  VerifyThreadState(rows_it, 10, 20, THREAD_A, kRunnable);
  VerifyThreadState(++rows_it, 10, 20, THREAD_B, kRunning);
}

TEST_F(ThreadStateTrackerUnittest, PushIdleThread) {
  tracker_->PushSchedSwitchEvent(10, CPU_A, IDLE_THREAD, StringIdOf(kRunnable),
                                 THREAD_A);
  auto rows_it = ThreadStateIterator();

  // The opening of idle_thred should be discarded so the first row will be
  // for the THREAD_A.
  VerifyThreadState(rows_it, 10, base::nullopt, THREAD_A, kRunning);
}

TEST_F(ThreadStateTrackerUnittest, SchedBlockedReasonWithIdleThread) {
  tracker_->PushSchedSwitchEvent(1, CPU_A, IDLE_THREAD, StringIdOf("D"),
                                 THREAD_A);
  tracker_->PushSchedSwitchEvent(2, CPU_A, THREAD_A, StringIdOf("D"),
                                 IDLE_THREAD);
  tracker_->PushBlockedReason(THREAD_A, IDLE_THREAD, base::nullopt);
  tracker_->PushSchedSwitchEvent(3, CPU_A, IDLE_THREAD, StringIdOf("D"),
                                 THREAD_B);
  tracker_->PushSchedSwitchEvent(4, CPU_A, THREAD_B, StringIdOf("D"),
                                 IDLE_THREAD);
  tracker_->PushBlockedReason(THREAD_B, 1, base::nullopt);

  auto rows_it = ThreadStateIterator();

  VerifyThreadState(rows_it, 1, 2, THREAD_A, kRunning);
  VerifyThreadState(++rows_it, 2, base::nullopt, THREAD_A, "D", 0);
  VerifyThreadState(++rows_it, 3, 4, THREAD_B, kRunning);
  VerifyThreadState(++rows_it, 4, base::nullopt, THREAD_B, "D", 1);
}

TEST_F(ThreadStateTrackerUnittest, SchedSwitchForcedMigration) {
  tracker_->PushSchedSwitchEvent(1, CPU_A, THREAD_A, StringIdOf("S"), THREAD_B);
  tracker_->PushSchedSwitchEvent(2, CPU_A, THREAD_A, StringIdOf("S"), THREAD_B);

  auto rows_it = ThreadStateIterator();
  VerifyThreadState(rows_it, 1, base::nullopt, THREAD_A, "S");
  VerifyThreadState(++rows_it, 1, 2, THREAD_B, kRunning);
}

TEST_F(ThreadStateTrackerUnittest, SchedWakingBigTest) {
  tracker_->PushWakingEvent(1, 8, 11);
  tracker_->PushSchedSwitchEvent(2, CPU_A, 11, StringIdOf("S"), 0);
  tracker_->PushSchedSwitchEvent(3, CPU_A, 8, StringIdOf("S"), 0);
  tracker_->PushSchedSwitchEvent(4, CPU_A, 17771, StringIdOf("S"), 17772);
  tracker_->PushSchedSwitchEvent(5, CPU_A, 17772, StringIdOf("S"), 0);
  tracker_->PushWakingEvent(6, 18, 0);
  tracker_->PushSchedSwitchEvent(7, CPU_A, 0, StringIdOf(kRunnable), 18);

  auto rows_it = ThreadStateIterator();
  VerifyThreadState(rows_it, 2, base::nullopt, 11, "S");
  VerifyThreadState(++rows_it, 3, base::nullopt, 8, "S");
  VerifyThreadState(++rows_it, 4, base::nullopt, 17771, "S");
  VerifyThreadState(++rows_it, 4, 5, 17772, kRunning);
  VerifyThreadState(++rows_it, 5, base::nullopt, 17772, "S");
  VerifyThreadState(++rows_it, 7, base::nullopt, 18, kRunning);
}

TEST_F(ThreadStateTrackerUnittest, RunningOnMultipleCPUsForcedMigration) {
  // Thread A was running on multiple CPUs
  tracker_->PushSchedSwitchEvent(1, CPU_A, THREAD_C, StringIdOf("S"), THREAD_A);
  tracker_->PushSchedSwitchEvent(2, CPU_B, THREAD_B, StringIdOf("S"), THREAD_A);

  auto rows_it = ThreadStateIterator();
  VerifyThreadState(rows_it, 1, base::nullopt, THREAD_C, "S");
  VerifyThreadState(++rows_it, 1, 2, THREAD_A, kRunning);
  VerifyThreadState(++rows_it, 2, base::nullopt, THREAD_B, "S");
  VerifyThreadState(++rows_it, 2, base::nullopt, THREAD_A, kRunning,
                    base::nullopt, base::nullopt, base::nullopt, CPU_B);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
