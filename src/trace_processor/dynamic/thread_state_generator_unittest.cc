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

#include "src/trace_processor/dynamic/thread_state_generator.h"

#include <algorithm>

#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

class ThreadStateGeneratorUnittest : public testing::Test {
 public:
  struct Ts {
    int64_t ts;
  };

  ThreadStateGeneratorUnittest() : idle_thread_(0), thread_a_(1), thread_b_(2) {
    context_.storage.reset(new TraceStorage());
    thread_state_generator_.reset(new ThreadStateGenerator(&context_));
  }

  void ForwardSchedTo(Ts ts) { sched_insert_ts_ = ts.ts; }

  void AddWaking(Ts ts, UniqueTid utid) {
    tables::InstantTable::Row row;
    row.ts = ts.ts;
    row.ref = utid;
    row.name = context_.storage->InternString("sched_waking");
    context_.storage->mutable_instant_table()->Insert(row);
  }

  void AddWakup(Ts ts, UniqueTid utid) {
    tables::InstantTable::Row row;
    row.ts = ts.ts;
    row.ref = utid;
    row.name = context_.storage->InternString("sched_wakeup");
    context_.storage->mutable_instant_table()->Insert(row);
  }

  void AddSched(base::Optional<Ts> end, UniqueTid utid, const char* end_state) {
    StringId end_state_id = context_.storage->InternString(end_state);

    tables::SchedSliceTable::Row row;

    // cpu is hardcoded because it doesn't matter for the algorithm and is
    // just passed through unchanged.
    row.cpu = 0;

    row.ts = sched_insert_ts_;
    row.dur = end ? end->ts - row.ts : -1;
    row.utid = utid;
    row.end_state = end_state_id;
    context_.storage->mutable_sched_slice_table()->Insert(row);

    sched_insert_ts_ = end ? end->ts : -1;
  }

  void RunThreadStateComputation() {
    thread_state_table_ = thread_state_generator_->ComputeThreadStateTable();
  }

  void VerifyThreadState(Ts from,
                         base::Optional<Ts> to,
                         UniqueTid utid,
                         const char* state) {
    uint32_t row = thread_state_verify_row_;

    ASSERT_LT(row, thread_state_table_->row_count());
    EXPECT_EQ(thread_state_table_->ts()[row], from.ts);
    EXPECT_EQ(thread_state_table_->dur()[row], to ? to->ts - from.ts : -1);
    EXPECT_EQ(thread_state_table_->utid()[row], utid);
    if (state == kRunning) {
      EXPECT_EQ(thread_state_table_->cpu()[row], 0u);
    } else {
      EXPECT_EQ(thread_state_table_->cpu()[row], base::nullopt);
    }
    EXPECT_EQ(thread_state_table_->state().GetString(row),
              base::StringView(state));

    thread_state_verify_row_++;
  }

  void VerifyEndOfThreadState() {
    ASSERT_EQ(thread_state_verify_row_, thread_state_table_->row_count());
  }

 protected:
  static constexpr char kRunning[] = "Running";

  const UniqueTid idle_thread_;
  const UniqueTid thread_a_;
  const UniqueTid thread_b_;

 private:
  TraceProcessorContext context_;

  int64_t sched_insert_ts_ = 0;

  uint32_t thread_state_verify_row_ = 0;

  std::unique_ptr<ThreadStateGenerator> thread_state_generator_;
  std::unique_ptr<tables::ThreadStateTable> thread_state_table_;
};

constexpr char ThreadStateGeneratorUnittest::kRunning[];

TEST_F(ThreadStateGeneratorUnittest, MultipleThreadWithOnlySched) {
  ForwardSchedTo(Ts{0});
  AddSched(Ts{10}, thread_a_, "S");
  AddSched(Ts{15}, thread_b_, "D");
  AddSched(Ts{20}, thread_a_, "R");

  RunThreadStateComputation();

  VerifyThreadState(Ts{0}, Ts{10}, thread_a_, kRunning);
  VerifyThreadState(Ts{10}, Ts{15}, thread_a_, "S");

  VerifyThreadState(Ts{10}, Ts{15}, thread_b_, kRunning);
  VerifyThreadState(Ts{15}, base::nullopt, thread_b_, "D");

  VerifyThreadState(Ts{15}, Ts{20}, thread_a_, kRunning);
  VerifyThreadState(Ts{20}, base::nullopt, thread_a_, "R");

  VerifyEndOfThreadState();
}

TEST_F(ThreadStateGeneratorUnittest, WakingFirst) {
  AddWaking(Ts{10}, thread_a_);

  ForwardSchedTo(Ts{20});
  AddSched(Ts{30}, thread_a_, "S");

  RunThreadStateComputation();

  VerifyThreadState(Ts{10}, Ts{20}, thread_a_, "R");
  VerifyThreadState(Ts{20}, Ts{30}, thread_a_, kRunning);
  VerifyThreadState(Ts{30}, base::nullopt, thread_a_, "S");

  VerifyEndOfThreadState();
}

TEST_F(ThreadStateGeneratorUnittest, SchedWithWaking) {
  ForwardSchedTo(Ts{0});
  AddSched(Ts{10}, thread_a_, "S");

  AddWaking(Ts{15}, thread_a_);

  ForwardSchedTo(Ts{20});
  AddSched(Ts{25}, thread_a_, "R");

  RunThreadStateComputation();

  VerifyThreadState(Ts{0}, Ts{10}, thread_a_, kRunning);
  VerifyThreadState(Ts{10}, Ts{15}, thread_a_, "S");
  VerifyThreadState(Ts{15}, Ts{20}, thread_a_, "R");
  VerifyThreadState(Ts{20}, Ts{25}, thread_a_, kRunning);
  VerifyThreadState(Ts{25}, base::nullopt, thread_a_, "R");

  VerifyEndOfThreadState();
}

TEST_F(ThreadStateGeneratorUnittest, SchedWithWakeup) {
  ForwardSchedTo(Ts{0});
  AddSched(Ts{10}, thread_a_, "S");

  AddWakup(Ts{15}, thread_a_);

  ForwardSchedTo(Ts{20});
  AddSched(Ts{25}, thread_a_, "R");

  RunThreadStateComputation();

  VerifyThreadState(Ts{0}, Ts{10}, thread_a_, kRunning);
  VerifyThreadState(Ts{10}, Ts{15}, thread_a_, "S");
  VerifyThreadState(Ts{15}, Ts{20}, thread_a_, "R");
  VerifyThreadState(Ts{20}, Ts{25}, thread_a_, kRunning);
  VerifyThreadState(Ts{25}, base::nullopt, thread_a_, "R");

  VerifyEndOfThreadState();
}

TEST_F(ThreadStateGeneratorUnittest, SchedIdleIgnored) {
  ForwardSchedTo(Ts{0});
  AddSched(Ts{10}, idle_thread_, "R");
  AddSched(Ts{15}, thread_a_, "R");

  RunThreadStateComputation();

  VerifyThreadState(Ts{10}, Ts{15}, thread_a_, kRunning);
  VerifyThreadState(Ts{15}, base::nullopt, thread_a_, "R");

  VerifyEndOfThreadState();
}

TEST_F(ThreadStateGeneratorUnittest, NegativeSchedDuration) {
  ForwardSchedTo(Ts{0});

  AddSched(Ts{10}, thread_a_, "S");

  AddWaking(Ts{15}, thread_a_);

  ForwardSchedTo(Ts{20});
  AddSched(base::nullopt, thread_a_, "");

  RunThreadStateComputation();

  VerifyThreadState(Ts{0}, Ts{10}, thread_a_, kRunning);
  VerifyThreadState(Ts{10}, Ts{15}, thread_a_, "S");
  VerifyThreadState(Ts{15}, Ts{20}, thread_a_, "R");
  VerifyThreadState(Ts{20}, base::nullopt, thread_a_, kRunning);

  VerifyEndOfThreadState();
}

TEST_F(ThreadStateGeneratorUnittest, WakingOnRunningThreadAtEnd) {
  AddWaking(Ts{5}, thread_a_);

  ForwardSchedTo(Ts{10});
  AddSched(base::nullopt, thread_a_, "");

  AddWaking(Ts{15}, thread_a_);

  RunThreadStateComputation();

  VerifyThreadState(Ts{5}, Ts{10}, thread_a_, "R");
  VerifyThreadState(Ts{10}, base::nullopt, thread_a_, kRunning);

  VerifyEndOfThreadState();
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
