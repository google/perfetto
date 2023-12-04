/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/common/clock_converter.h"
#include "src/trace_processor/importers/common/clock_tracker.h"

#include <random>

#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {

class ClockConverterTest : public ::testing::Test {
 public:
  ClockConverterTest() { context_.storage.reset(new TraceStorage()); }

  TraceProcessorContext context_;
  ClockConverter cc_{&context_};
};

namespace {

using ::testing::NiceMock;
using Clock = protos::pbzero::ClockSnapshot::Clock;

static constexpr int64_t kMonotonic =
    protos::pbzero::BuiltinClock::BUILTIN_CLOCK_MONOTONIC;
static constexpr int64_t kReal = protos::pbzero::ClockSnapshot::Clock::REALTIME;

TEST_F(ClockConverterTest, EmptyTable) {
  EXPECT_FALSE(cc_.ToAbsTime(10).ok());
  EXPECT_FALSE(cc_.ToMonotonic(10).ok());
}

TEST_F(ClockConverterTest, TrivialMonotonic) {
  tables::ClockSnapshotTable::Row row;
  row.ts = 10;
  row.clock_id = kMonotonic;
  row.clock_value = 20;
  context_.storage->mutable_clock_snapshot_table()->Insert(row);

  EXPECT_TRUE(cc_.ToMonotonic(10).ok());
  EXPECT_EQ(cc_.ToMonotonic(10).value(), 20);
}

TEST_F(ClockConverterTest, TrivialToRealtime) {
  tables::ClockSnapshotTable::Row row;
  row.ts = 10;
  row.clock_id = kReal;
  row.clock_value = 20;
  context_.storage->mutable_clock_snapshot_table()->Insert(row);

  EXPECT_TRUE(cc_.ToRealtime(10).ok());
  EXPECT_EQ(cc_.ToRealtime(10).value(), 20);
}

TEST_F(ClockConverterTest, TrivialToAbsTime) {
  tables::ClockSnapshotTable::Row row;
  row.ts = 10;
  row.clock_id = kReal;
  row.clock_value = 20;
  context_.storage->mutable_clock_snapshot_table()->Insert(row);

  EXPECT_TRUE(cc_.ToAbsTime(10).ok());
  EXPECT_EQ(cc_.ToAbsTime(10).value(), "1970-01-01T00:00:00.000000020");
}

TEST_F(ClockConverterTest, Monotonic) {
  {
    tables::ClockSnapshotTable::Row rows;
    rows.ts = 10;
    rows.clock_id = kMonotonic;
    rows.clock_value = 10;
    context_.storage->mutable_clock_snapshot_table()->Insert(rows);
  }
  {
    tables::ClockSnapshotTable::Row rows;
    rows.ts = 20;
    rows.clock_id = kMonotonic;
    rows.clock_value = 10;
    context_.storage->mutable_clock_snapshot_table()->Insert(rows);
  }
  {
    tables::ClockSnapshotTable::Row rows;
    rows.ts = 30;
    rows.clock_id = kMonotonic;
    rows.clock_value = 20;
    context_.storage->mutable_clock_snapshot_table()->Insert(rows);
  }
  {
    tables::ClockSnapshotTable::Row rows;
    rows.ts = 40;
    rows.clock_id = kMonotonic;
    rows.clock_value = 20;
    context_.storage->mutable_clock_snapshot_table()->Insert(rows);
  }

  EXPECT_EQ(cc_.ToMonotonic(15).value(), 10);
  EXPECT_EQ(cc_.ToMonotonic(25).value(), 15);
  EXPECT_EQ(cc_.ToMonotonic(35).value(), 20);
  EXPECT_EQ(cc_.ToMonotonic(45).value(), 25);
}

TEST_F(ClockConverterTest, Realtime) {
  // We will add 3 snapshots for real time clock, and the last snapshot will be
  // earlier then the second one.
  {
    tables::ClockSnapshotTable::Row rows;
    rows.ts = 10;
    rows.clock_id = kReal;
    rows.clock_value = 0;
    context_.storage->mutable_clock_snapshot_table()->Insert(rows);
  }
  {
    tables::ClockSnapshotTable::Row rows;
    rows.ts = 20;
    rows.clock_id = kReal;
    rows.clock_value = 10;
    context_.storage->mutable_clock_snapshot_table()->Insert(rows);
  }
  {
    tables::ClockSnapshotTable::Row rows;
    rows.ts = 30;
    rows.clock_id = kReal;
    rows.clock_value = 5;
    context_.storage->mutable_clock_snapshot_table()->Insert(rows);
  }

  EXPECT_EQ(cc_.ToRealtime(15).value(), 5);
  EXPECT_EQ(cc_.ToRealtime(25).value(), 5);
  EXPECT_EQ(cc_.ToRealtime(35).value(), 10);
}

TEST_F(ClockConverterTest, AbsTime) {
  // We will add 3 snapshots for real time clock, and the last snapshot will be
  // earlier then the second one.
  {
    tables::ClockSnapshotTable::Row rows;
    rows.ts = 10;
    rows.clock_id = kReal;
    rows.clock_value = 0;
    context_.storage->mutable_clock_snapshot_table()->Insert(rows);
  }
  {
    tables::ClockSnapshotTable::Row rows;
    rows.ts = 20;
    rows.clock_id = kReal;
    rows.clock_value = 1652904000000000000;
    context_.storage->mutable_clock_snapshot_table()->Insert(rows);
  }
  {
    tables::ClockSnapshotTable::Row rows;
    rows.ts = 30;
    rows.clock_id = kReal;
    rows.clock_value = 1652904000000000000 - 5;
    context_.storage->mutable_clock_snapshot_table()->Insert(rows);
  }

  EXPECT_EQ(cc_.ToAbsTime(15).value(), "1970-01-01T00:00:00.000000005");
  EXPECT_EQ(cc_.ToAbsTime(25).value(), "2022-05-18T19:59:59.999999995");
  EXPECT_EQ(cc_.ToAbsTime(35).value(), "2022-05-18T20:00:00.000000000");
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
