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
#include "src/trace_processor/storage/trace_storage.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using MachineId = tables::MachineTable::Id;
using TraceId = tables::TraceFileTable::Id;

class GlobalStatsTrackerTest : public ::testing::Test {
 protected:
  TraceStorage storage_;
  GlobalStatsTracker tracker_{&storage_};
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

// Writing the same stat to two distinct (machine, trace) contexts produces
// rows in both contexts. We verify that GetStats sees both written values
// back, scoped per-context.
TEST_F(GlobalStatsTrackerTest, EachContextProducesOneRow) {
  tracker_.SetStats(MachineId(1), TraceId(1), stats::android_log_num_failed,
                    10);
  tracker_.SetStats(MachineId(2), TraceId(1), stats::android_log_num_failed,
                    20);

  EXPECT_EQ(tracker_.GetStats(MachineId(1), TraceId(1),
                              stats::android_log_num_failed),
            10);
  EXPECT_EQ(tracker_.GetStats(MachineId(2), TraceId(1),
                              stats::android_log_num_failed),
            20);
}

// SetStats updates the existing row rather than inserting a new one. The
// table's row_count must stay constant across the second write — the new
// value lands on the row materialized on the first write.
TEST_F(GlobalStatsTrackerTest, SetStatsUpdatesExistingRow) {
  tracker_.SetStats(MachineId(1), TraceId(1), stats::android_log_num_failed, 7);
  uint32_t row_count_after_first = storage_.stats_table().row_count();
  tracker_.SetStats(MachineId(1), TraceId(1), stats::android_log_num_failed, 9);
  EXPECT_EQ(storage_.stats_table().row_count(), row_count_after_first);
  EXPECT_EQ(tracker_.GetStats(MachineId(1), TraceId(1),
                              stats::android_log_num_failed),
            9);
}

// kMachineAndTrace stats require BOTH machine_id and trace_id to be set;
// passing nullopt for either should crash via PERFETTO_CHECK. This documents
// (and pins) the strictness, and matches GlobalMetadataTracker's contract.
TEST_F(GlobalStatsTrackerTest, MachineAndTraceRequiresMachineId) {
  EXPECT_DEATH_IF_SUPPORTED(tracker_.SetStats(std::nullopt, TraceId(1),
                                              stats::android_log_num_failed, 1),
                            "");
}

TEST_F(GlobalStatsTrackerTest, MachineAndTraceRequiresTraceId) {
  EXPECT_DEATH_IF_SUPPORTED(tracker_.SetStats(MachineId(1), std::nullopt,
                                              stats::android_log_num_failed, 1),
                            "");
}

// Calling SetStats on an indexed-type key, or SetIndexedStats on a
// single-type key, is a programmer error and must crash.
TEST_F(GlobalStatsTrackerTest, SetStatsOnIndexedKeyCrashes) {
  EXPECT_DEATH_IF_SUPPORTED(tracker_.SetStats(MachineId(1), TraceId(1),
                                              stats::ftrace_cpu_bytes_begin, 1),
                            "");
}

TEST_F(GlobalStatsTrackerTest, SetIndexedStatsOnSingleKeyCrashes) {
  EXPECT_DEATH_IF_SUPPORTED(
      tracker_.SetIndexedStats(MachineId(1), TraceId(1),
                               stats::android_log_num_failed,
                               /*index=*/0, 1),
      "");
}

// kMachine and kTrace scopes are declared in stats.h but currently no real
// stat uses them (every stat is either kGlobal or kMachineAndTrace). If a
// future stat uses one of these scopes, GlobalStatsTracker should still
// CHECK that the corresponding context id is provided. This test documents
// that no stat currently uses them, so the corresponding CHECK paths in
// GetContextKey are unreachable from production today; if you hit this
// failure, add coverage for the new scope along with the new stat.
TEST_F(GlobalStatsTrackerTest, NoStatUsesKMachineOrKTraceScope) {
  for (size_t i = 0; i < stats::kNumKeys; ++i) {
    EXPECT_NE(stats::kScopes[i], stats::Scope::kMachine)
        << "Stat #" << i << " (" << stats::kNames[i]
        << ") uses kMachine scope; add a death test for missing machine_id.";
    EXPECT_NE(stats::kScopes[i], stats::Scope::kTrace)
        << "Stat #" << i << " (" << stats::kNames[i]
        << ") uses kTrace scope; add a death test for missing trace_id.";
  }
}

}  // namespace
}  // namespace perfetto::trace_processor
