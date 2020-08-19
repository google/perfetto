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

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
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
    context_.global_args_tracker.reset(new GlobalArgsTracker(&context_));
    context_.args_tracker.reset(new ArgsTracker(&context_));
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

  void AddBlockedReason(Ts ts, UniqueTid utid, bool io_wait) {
    tables::InstantTable::Row row;
    row.ts = ts.ts;
    row.ref = utid;
    row.name = context_.storage->InternString("sched_blocked_reason");

    auto id = context_.storage->mutable_instant_table()->Insert(row).id;
    auto inserter = context_.args_tracker->AddArgsTo(id);
    inserter.AddArg(context_.storage->InternString("io_wait"),
                    Variadic::Boolean(io_wait));
    context_.args_tracker->Flush();
  }

  void RunThreadStateComputation(Ts trace_end_ts = Ts{
                                     std::numeric_limits<int64_t>::max()}) {
    unsorted_table_ =
        thread_state_generator_->ComputeThreadStateTable(trace_end_ts.ts);
    table_.reset(
        new Table(unsorted_table_->Sort({unsorted_table_->ts().ascending()})));
  }

  void VerifyThreadState(Ts from,
                         base::Optional<Ts> to,
                         UniqueTid utid,
                         const char* state,
                         base::Optional<bool> io_wait = base::nullopt) {
    uint32_t row = thread_state_verify_row_++;

    const auto& ts_col = table_->GetTypedColumnByName<int64_t>("ts");
    const auto& dur_col = table_->GetTypedColumnByName<int64_t>("dur");
    const auto& utid_col = table_->GetTypedColumnByName<UniqueTid>("utid");
    const auto& cpu_col =
        table_->GetTypedColumnByName<base::Optional<uint32_t>>("cpu");
    const auto& end_state_col = table_->GetTypedColumnByName<StringId>("state");
    const auto& io_wait_col =
        table_->GetTypedColumnByName<base::Optional<uint32_t>>("io_wait");

    ASSERT_LT(row, table_->row_count());
    ASSERT_EQ(ts_col[row], from.ts);
    ASSERT_EQ(dur_col[row], to ? to->ts - from.ts : -1);
    ASSERT_EQ(utid_col[row], utid);
    if (state == kRunning) {
      ASSERT_EQ(cpu_col[row], 0u);
    } else {
      ASSERT_EQ(cpu_col[row], base::nullopt);
    }
    ASSERT_EQ(end_state_col.GetString(row), base::StringView(state));

    base::Optional<uint32_t> mapped_io_wait =
        io_wait ? base::make_optional(static_cast<uint32_t>(*io_wait))
                : base::nullopt;
    ASSERT_EQ(io_wait_col[row], mapped_io_wait);
  }

  void VerifyEndOfThreadState() {
    ASSERT_EQ(thread_state_verify_row_, table_->row_count());
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
  std::unique_ptr<tables::ThreadStateTable> unsorted_table_;
  std::unique_ptr<Table> table_;
};

constexpr char ThreadStateGeneratorUnittest::kRunning[];

TEST_F(ThreadStateGeneratorUnittest, MultipleThreadWithOnlySched) {
  ForwardSchedTo(Ts{0});
  AddSched(Ts{10}, thread_a_, "S");
  AddSched(Ts{15}, thread_b_, "D");
  AddSched(Ts{20}, thread_a_, "R");

  RunThreadStateComputation();

  VerifyThreadState(Ts{0}, Ts{10}, thread_a_, kRunning);
  VerifyThreadState(Ts{10}, Ts{15}, thread_b_, kRunning);
  VerifyThreadState(Ts{10}, Ts{15}, thread_a_, "S");
  VerifyThreadState(Ts{15}, Ts{20}, thread_a_, kRunning);
  VerifyThreadState(Ts{15}, base::nullopt, thread_b_, "D");
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
  VerifyThreadState(Ts{15}, base::nullopt, thread_a_, "R");

  VerifyEndOfThreadState();
}

TEST_F(ThreadStateGeneratorUnittest, SchedDataLoss) {
  ForwardSchedTo(Ts{10});
  AddSched(base::nullopt, thread_a_, "");
  ForwardSchedTo(Ts{30});
  AddSched(Ts{40}, thread_a_, "D");

  RunThreadStateComputation();

  VerifyThreadState(Ts{10}, base::nullopt, thread_a_, kRunning);
  VerifyThreadState(Ts{30}, Ts{40}, thread_a_, kRunning);
  VerifyThreadState(Ts{40}, base::nullopt, thread_a_, "D");

  VerifyEndOfThreadState();
}

TEST_F(ThreadStateGeneratorUnittest, StrechedSchedIgnored) {
  ForwardSchedTo(Ts{10});
  AddSched(Ts{100}, thread_a_, "");

  RunThreadStateComputation(Ts{100});

  VerifyThreadState(Ts{10}, base::nullopt, thread_a_, kRunning);

  VerifyEndOfThreadState();
}

TEST_F(ThreadStateGeneratorUnittest, WakingAfterStrechedSched) {
  ForwardSchedTo(Ts{10});
  AddSched(Ts{100}, thread_a_, "");

  AddWaking(Ts{15}, thread_a_);

  RunThreadStateComputation(Ts{100});

  VerifyThreadState(Ts{10}, base::nullopt, thread_a_, kRunning);
  VerifyThreadState(Ts{15}, base::nullopt, thread_a_, "R");

  VerifyEndOfThreadState();
}

TEST_F(ThreadStateGeneratorUnittest, BlockedReason) {
  ForwardSchedTo(Ts{10});
  AddSched(Ts{12}, thread_a_, "D");
  AddWaking(Ts{15}, thread_a_);
  AddBlockedReason(Ts{16}, thread_a_, true);

  ForwardSchedTo(Ts{18});
  AddSched(Ts{20}, thread_a_, "S");
  AddWaking(Ts{24}, thread_a_);
  AddBlockedReason(Ts{26}, thread_a_, false);

  ForwardSchedTo(Ts{29});
  AddSched(Ts{30}, thread_a_, "R");

  ForwardSchedTo(Ts{39});
  AddSched(Ts{40}, thread_a_, "D");
  AddBlockedReason(Ts{44}, thread_a_, false);

  ForwardSchedTo(Ts{49});
  AddSched(Ts{50}, thread_a_, "D");

  RunThreadStateComputation();

  VerifyThreadState(Ts{10}, Ts{12}, thread_a_, kRunning);
  VerifyThreadState(Ts{12}, Ts{15}, thread_a_, "D", true);
  VerifyThreadState(Ts{15}, Ts{18}, thread_a_, "R");

  VerifyThreadState(Ts{18}, Ts{20}, thread_a_, kRunning);
  VerifyThreadState(Ts{20}, Ts{24}, thread_a_, "S", false);
  VerifyThreadState(Ts{24}, Ts{29}, thread_a_, "R");

  VerifyThreadState(Ts{29}, Ts{30}, thread_a_, kRunning);
  VerifyThreadState(Ts{30}, Ts{39}, thread_a_, "R", base::nullopt);

  VerifyThreadState(Ts{39}, Ts{40}, thread_a_, kRunning);
  VerifyThreadState(Ts{40}, Ts{49}, thread_a_, "D", false);

  VerifyThreadState(Ts{49}, Ts{50}, thread_a_, kRunning);
  VerifyThreadState(Ts{50}, base::nullopt, thread_a_, "D");

  VerifyEndOfThreadState();
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
