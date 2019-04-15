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

#include "src/trace_processor/syscall_tracker.h"

#include "src/trace_processor/slice_tracker.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::_;
using ::testing::SaveArg;

class MockSliceTracker : public SliceTracker {
 public:
  MockSliceTracker(TraceProcessorContext* context) : SliceTracker(context) {}
  virtual ~MockSliceTracker() = default;

  MOCK_METHOD4(
      Begin,
      void(int64_t timestamp, UniqueTid utid, StringId cat, StringId name));
  MOCK_METHOD4(
      End,
      void(int64_t timestamp, UniqueTid utid, StringId cat, StringId name));
};

class SyscallTrackerTest : public ::testing::Test {
 public:
  SyscallTrackerTest() {
    slice_tracker = new MockSliceTracker(&context);
    context.storage.reset(new TraceStorage());
    context.slice_tracker.reset(slice_tracker);
    context.syscall_tracker.reset(new SyscallTracker(&context));
  }

 protected:
  TraceProcessorContext context;
  MockSliceTracker* slice_tracker;
};

TEST_F(SyscallTrackerTest, ReportUnknownSyscalls) {
  StringId begin_name = 0;
  StringId end_name = 0;
  EXPECT_CALL(*slice_tracker, Begin(100, 42, 0, _))
      .WillOnce(SaveArg<3>(&begin_name));
  EXPECT_CALL(*slice_tracker, End(110, 42, 0, _))
      .WillOnce(SaveArg<3>(&end_name));

  context.syscall_tracker->Enter(100 /*ts*/, 42 /*utid*/, 57 /*sys_read*/);
  context.syscall_tracker->Exit(110 /*ts*/, 42 /*utid*/, 57 /*sys_read*/);
  EXPECT_EQ(context.storage->GetString(begin_name), "sys_57");
  EXPECT_EQ(context.storage->GetString(end_name), "sys_57");
}

TEST_F(SyscallTrackerTest, IgnoreWriteSyscalls) {
  context.syscall_tracker->SetArchitecture(kAarch64);
  EXPECT_CALL(*slice_tracker, Begin(_, _, _, _)).Times(0);
  EXPECT_CALL(*slice_tracker, End(_, _, _, _)).Times(0);

  context.syscall_tracker->Enter(100 /*ts*/, 42 /*utid*/, 64 /*sys_write*/);
  context.syscall_tracker->Exit(110 /*ts*/, 42 /*utid*/, 64 /*sys_write*/);
}

TEST_F(SyscallTrackerTest, Aarch64) {
  StringId begin_name = 0;
  StringId end_name = 0;
  EXPECT_CALL(*slice_tracker, Begin(100, 42, 0, _))
      .WillOnce(SaveArg<3>(&begin_name));
  EXPECT_CALL(*slice_tracker, End(110, 42, 0, _))
      .WillOnce(SaveArg<3>(&end_name));

  context.syscall_tracker->SetArchitecture(kAarch64);
  context.syscall_tracker->Enter(100 /*ts*/, 42 /*utid*/, 63 /*sys_read*/);
  context.syscall_tracker->Exit(110 /*ts*/, 42 /*utid*/, 63 /*sys_read*/);
  EXPECT_EQ(context.storage->GetString(begin_name), "sys_read");
  EXPECT_EQ(context.storage->GetString(end_name), "sys_read");
}

TEST_F(SyscallTrackerTest, x8664) {
  StringId begin_name = 0;
  StringId end_name = 0;
  EXPECT_CALL(*slice_tracker, Begin(100, 42, 0, _))
      .WillOnce(SaveArg<3>(&begin_name));
  EXPECT_CALL(*slice_tracker, End(110, 42, 0, _))
      .WillOnce(SaveArg<3>(&end_name));

  context.syscall_tracker->SetArchitecture(kX86_64);
  context.syscall_tracker->Enter(100 /*ts*/, 42 /*utid*/, 0 /*sys_read*/);
  context.syscall_tracker->Exit(110 /*ts*/, 42 /*utid*/, 0 /*sys_read*/);
  EXPECT_EQ(context.storage->GetString(begin_name), "sys_read");
  EXPECT_EQ(context.storage->GetString(end_name), "sys_read");
}

TEST_F(SyscallTrackerTest, SyscallNumberTooLarge) {
  EXPECT_CALL(*slice_tracker, Begin(_, _, _, _)).Times(0);
  EXPECT_CALL(*slice_tracker, End(_, _, _, _)).Times(0);
  context.syscall_tracker->SetArchitecture(kAarch64);
  context.syscall_tracker->Enter(100 /*ts*/, 42 /*utid*/, 9999);
  context.syscall_tracker->Exit(110 /*ts*/, 42 /*utid*/, 9999);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
