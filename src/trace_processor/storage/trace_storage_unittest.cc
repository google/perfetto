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

#include "src/trace_processor/storage/trace_storage.h"

#include <cstdint>
#include <string>

#include "src/trace_processor/util/concurrency_stress_test_util.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

TEST(TraceStorageTest, SqlStatsConcurrentRecord) {
  TraceStorage storage;
  TraceStorage::SqlStats& stats = *storage.mutable_sql_stats();

  constexpr uint32_t kThreads = 8;
  constexpr uint32_t kIterations = 2000;

  // Each thread records a query begin (with a thread-unique, well-formed query
  // string), then its first-next and end. Under the multi-connection design
  // these run concurrently on the same shared SqlStats; without a lock the
  // deque push_back/pop_front race (caught by TSan and visible as torn state).
  ConcurrentlyRun(kThreads, kIterations, [&](uint32_t thread_idx) {
    std::string query = "SELECT " + std::to_string(thread_idx);
    uint32_t row = stats.RecordQueryBegin(query, /*time_started=*/100);
    stats.RecordQueryFirstNext(row, /*time_first_next=*/200);
    stats.RecordQueryEnd(row, /*time_ended=*/300);
  });

  // The bounded log must never exceed its cap.
  ASSERT_LE(stats.size(), TraceStorage::SqlStats::kMaxLogEntries);

  // All four deques must remain length-consistent.
  ASSERT_EQ(stats.size(), stats.queries().size());
  ASSERT_EQ(stats.size(), stats.times_started().size());
  ASSERT_EQ(stats.size(), stats.times_first_next().size());
  ASSERT_EQ(stats.size(), stats.times_ended().size());

  // Every retained query string must be one of the well-formed strings that
  // were written. A torn std::string (from an unsynchronized push_back) would
  // not match any of these.
  for (const std::string& query : stats.queries()) {
    bool matched = false;
    for (uint32_t t = 0; t < kThreads; ++t) {
      if (query == "SELECT " + std::to_string(t)) {
        matched = true;
        break;
      }
    }
    EXPECT_TRUE(matched) << "unexpected/torn query string: " << query;
  }
}

// GetSnapshot() must return an internally-consistent, race-free view even while
// other threads are concurrently recording. This is the read path the sql_stats
// table relies on under the multi-connection design.
TEST(TraceStorageTest, SqlStatsConcurrentSnapshotWhileRecording) {
  TraceStorage storage;
  TraceStorage::SqlStats& stats = *storage.mutable_sql_stats();

  constexpr uint32_t kThreads = 8;
  constexpr uint32_t kIterations = 2000;

  ConcurrentlyRun(kThreads, kIterations, [&](uint32_t thread_idx) {
    if (thread_idx % 2 == 0) {
      std::string query = "SELECT " + std::to_string(thread_idx);
      uint32_t row = stats.RecordQueryBegin(query, /*time_started=*/100);
      stats.RecordQueryEnd(row, /*time_ended=*/300);
    } else {
      TraceStorage::SqlStats::Snapshot snap = stats.GetSnapshot();
      // The snapshot is taken atomically, so all four columns are the same
      // length and every query string is intact.
      ASSERT_EQ(snap.queries.size(), snap.times_started.size());
      ASSERT_EQ(snap.queries.size(), snap.times_first_next.size());
      ASSERT_EQ(snap.queries.size(), snap.times_ended.size());
      for (const std::string& query : snap.queries)
        PERFETTO_CHECK(query.rfind("SELECT ", 0) == 0);
    }
  });
}

}  // namespace
}  // namespace perfetto::trace_processor
