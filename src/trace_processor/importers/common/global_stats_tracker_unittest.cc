/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/importers/common/global_stats_tracker.h"

#include <optional>

#include "src/trace_processor/storage/stats.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using MachineId = tables::MachineTable::Id;
using TraceId = tables::TraceFileTable::Id;

class GlobalStatsTrackerTest : public ::testing::Test {
 protected:
  GlobalStatsTracker tracker_;
};

// Verify that kScopes array has the correct number of entries.
TEST_F(GlobalStatsTrackerTest, ScopesArraySize) {
  static_assert(std::size(stats::kScopes) == stats::kNumKeys,
                "kScopes must have kNumKeys entries");
}

// kGlobal stats are stored with null machine_id and null trace_id,
// regardless of what context ids are passed.
TEST_F(GlobalStatsTrackerTest, GlobalStatIgnoresContext) {
  // guess_trace_type_duration_ns is kGlobal scope.
  auto key = stats::guess_trace_type_duration_ns;

  // Set with explicit machine_id and trace_id - should be ignored.
  tracker_.SetStats(MachineId(1), TraceId(2), key, 42);

  // Get with different context - still returns the value because scope
  // is kGlobal.
  EXPECT_EQ(tracker_.GetStats(std::nullopt, std::nullopt, key), 42);
  EXPECT_EQ(tracker_.GetStats(MachineId(99), TraceId(99), key), 42);
}

// kMachineAndTrace stats are stored per (machine_id, trace_id) pair.
TEST_F(GlobalStatsTrackerTest, MachineAndTraceStatPerContext) {
  // android_log_num_failed is kMachineAndTrace scope.
  auto key = stats::android_log_num_failed;

  tracker_.SetStats(MachineId(1), TraceId(1), key, 10);
  tracker_.SetStats(MachineId(2), TraceId(1), key, 20);

  EXPECT_EQ(tracker_.GetStats(MachineId(1), TraceId(1), key), 10);
  EXPECT_EQ(tracker_.GetStats(MachineId(2), TraceId(1), key), 20);
}

TEST_F(GlobalStatsTrackerTest, IncrementStats) {
  auto key = stats::android_log_num_total;

  tracker_.IncrementStats(MachineId(1), TraceId(1), key, 5);
  tracker_.IncrementStats(MachineId(1), TraceId(1), key, 3);

  EXPECT_EQ(tracker_.GetStats(MachineId(1), TraceId(1), key), 8);
}

TEST_F(GlobalStatsTrackerTest, IndexedStats) {
  // ftrace_cpu_bytes_begin is kIndexed, kMachineAndTrace.
  auto key = stats::ftrace_cpu_bytes_begin;

  tracker_.SetIndexedStats(MachineId(1), TraceId(1), key, /*index=*/0, 100);
  tracker_.SetIndexedStats(MachineId(1), TraceId(1), key, /*index=*/1, 200);

  EXPECT_EQ(tracker_.GetIndexedStats(MachineId(1), TraceId(1), key, 0), 100);
  EXPECT_EQ(tracker_.GetIndexedStats(MachineId(1), TraceId(1), key, 1), 200);
  EXPECT_EQ(tracker_.GetIndexedStats(MachineId(1), TraceId(1), key, 2),
            std::nullopt);
}

TEST_F(GlobalStatsTrackerTest, IncrementIndexedStats) {
  auto key = stats::ftrace_cpu_bytes_begin;

  tracker_.IncrementIndexedStats(MachineId(1), TraceId(1), key, /*index=*/0, 5);
  tracker_.IncrementIndexedStats(MachineId(1), TraceId(1), key, /*index=*/0, 3);

  EXPECT_EQ(tracker_.GetIndexedStats(MachineId(1), TraceId(1), key, 0), 8);
}

TEST_F(GlobalStatsTrackerTest, IndexedStatsPerContext) {
  auto key = stats::ftrace_cpu_bytes_begin;

  tracker_.SetIndexedStats(MachineId(1), TraceId(1), key, /*index=*/0, 100);
  tracker_.SetIndexedStats(MachineId(2), TraceId(1), key, /*index=*/0, 200);

  EXPECT_EQ(tracker_.GetIndexedStats(MachineId(1), TraceId(1), key, 0), 100);
  EXPECT_EQ(tracker_.GetIndexedStats(MachineId(2), TraceId(1), key, 0), 200);
}

TEST_F(GlobalStatsTrackerTest, GetStatsReturnsZeroForUnset) {
  auto key = stats::android_log_num_total;
  EXPECT_EQ(tracker_.GetStats(MachineId(1), TraceId(1), key), 0);
}

TEST_F(GlobalStatsTrackerTest, ContextKeysReturnsAllContexts) {
  tracker_.SetStats(MachineId(1), TraceId(1), stats::android_log_num_failed,
                    10);
  tracker_.SetStats(MachineId(2), TraceId(1), stats::android_log_num_failed,
                    20);

  auto keys = tracker_.context_keys();
  EXPECT_EQ(keys.size(), 2u);
}

}  // namespace
}  // namespace perfetto::trace_processor
