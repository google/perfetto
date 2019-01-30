/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/clock_tracker.h"
#include "perfetto/base/optional.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::NiceMock;
class MockTraceStorage : public TraceStorage {
 public:
  MockTraceStorage() : TraceStorage() {}
};

TEST(ClockTrackerTest, ClockDomainConversions) {
  TraceProcessorContext context;
  context.storage.reset(new NiceMock<MockTraceStorage>());
  ClockTracker ct(&context);

  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 0), base::nullopt);

  ct.SyncClocks(ClockDomain::kRealTime, 10, 10010);
  ct.SyncClocks(ClockDomain::kRealTime, 20, 20220);
  ct.SyncClocks(ClockDomain::kRealTime, 30, 30030);
  ct.SyncClocks(ClockDomain::kMonotonic, 1000, 100000);

  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 0), 10000);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 1), 10001);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 9), 10009);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 10), 10010);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 11), 10011);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 19), 10019);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 20), 20220);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 21), 20221);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 29), 20229);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 30), 30030);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 40), 30040);

  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kMonotonic, 0), 100000 - 1000);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kMonotonic, 999), 100000 - 1);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kMonotonic, 1000), 100000);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kMonotonic, 1e6),
            static_cast<int64_t>(100000 - 1000 + 1e6));
}

TEST(ClockTrackerTest, RealTimeClockMovingBackwards) {
  TraceProcessorContext context;
  ClockTracker ct(&context);

  ct.SyncClocks(ClockDomain::kRealTime, 10, 10010);
  ct.SyncClocks(ClockDomain::kRealTime, 20, 10020);
  ct.SyncClocks(ClockDomain::kRealTime, 40, 30040);
  ct.SyncClocks(ClockDomain::kRealTime, 30, 40030);

  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 11), 10011);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 29), 10029);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 30), 40030);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 40), 40040);

  ct.SyncClocks(ClockDomain::kRealTime, 50, 50000);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 55), 50005);

  ct.SyncClocks(ClockDomain::kRealTime, 11, 60011);
  EXPECT_EQ(ct.ToTraceTime(ClockDomain::kRealTime, 20), 60020);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
