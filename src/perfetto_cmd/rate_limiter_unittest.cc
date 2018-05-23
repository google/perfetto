/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/perfetto_cmd/rate_limiter.h"

#include <stdio.h>

#include "perfetto/base/scoped_file.h"
#include "perfetto/base/temp_file.h"
#include "perfetto/base/utils.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::NiceMock;
using testing::StrictMock;
using testing::Invoke;
using testing::Return;

namespace perfetto {

namespace {

class MockRateLimiter : public RateLimiter {
 public:
  MockRateLimiter() : dir_(base::TempDir::Create()) {
    ON_CALL(*this, LoadState(_))
        .WillByDefault(Invoke(this, &MockRateLimiter::LoadStateConcrete));
    ON_CALL(*this, SaveState(_))
        .WillByDefault(Invoke(this, &MockRateLimiter::SaveStateConcrete));
  }

  virtual std::string GetStateFilePath() const {
    return std::string(dir_.path()) + "/.guardraildata";
  }

  virtual ~MockRateLimiter() override {
    if (StateFileExists())
      remove(GetStateFilePath().c_str());
  }

  bool LoadStateConcrete(PerfettoCmdState* state) {
    return RateLimiter::LoadState(state);
  }

  bool SaveStateConcrete(const PerfettoCmdState& state) {
    return RateLimiter::SaveState(state);
  }

  MOCK_METHOD1(LoadState, bool(PerfettoCmdState*));
  MOCK_METHOD1(SaveState, bool(const PerfettoCmdState&));

 private:
  base::TempDir dir_;
};

void WriteGarbageToFile(const std::string& path) {
  base::ScopedFile fd;
  fd.reset(open(path.c_str(), O_WRONLY | O_CREAT, 0600));
  constexpr char data[] = "Some random bytes.";
  if (write(fd.get(), data, sizeof(data)) != sizeof(data))
    ADD_FAILURE() << "Could not write garbage";
}

TEST(RateLimiterTest, RoundTripState) {
  NiceMock<MockRateLimiter> limiter;

  PerfettoCmdState input{};
  PerfettoCmdState output{};

  input.set_total_bytes_uploaded(42);
  ASSERT_TRUE(limiter.SaveState(input));
  ASSERT_TRUE(limiter.LoadState(&output));
  ASSERT_EQ(output.total_bytes_uploaded(), 42u);
}

TEST(RateLimiterTest, LoadFromEmpty) {
  NiceMock<MockRateLimiter> limiter;

  PerfettoCmdState input{};
  input.set_total_bytes_uploaded(0);
  input.set_last_trace_timestamp(0);
  input.set_first_trace_timestamp(0);
  PerfettoCmdState output{};

  ASSERT_TRUE(limiter.SaveState(input));
  ASSERT_TRUE(limiter.LoadState(&output));
  ASSERT_EQ(output.total_bytes_uploaded(), 0u);
}

TEST(RateLimiterTest, LoadFromNoFileFails) {
  NiceMock<MockRateLimiter> limiter;
  PerfettoCmdState output{};
  ASSERT_FALSE(limiter.LoadState(&output));
  ASSERT_EQ(output.total_bytes_uploaded(), 0u);
}

TEST(RateLimiterTest, LoadFromGarbageFails) {
  NiceMock<MockRateLimiter> limiter;

  WriteGarbageToFile(limiter.GetStateFilePath().c_str());

  PerfettoCmdState output{};
  ASSERT_FALSE(limiter.LoadState(&output));
  ASSERT_EQ(output.total_bytes_uploaded(), 0u);
}

TEST(RateLimiterTest, NotDropBox) {
  StrictMock<MockRateLimiter> limiter;

  ASSERT_TRUE(limiter.ShouldTrace({}));
  ASSERT_TRUE(limiter.OnTraceDone({}, true, 10000));
  ASSERT_FALSE(limiter.StateFileExists());
}

TEST(RateLimiterTest, NotDropBox_FailedToTrace) {
  StrictMock<MockRateLimiter> limiter;

  ASSERT_FALSE(limiter.OnTraceDone({}, false, 0));
  ASSERT_FALSE(limiter.StateFileExists());
}

TEST(RateLimiterTest, DropBox_IgnoreGuardrails) {
  StrictMock<MockRateLimiter> limiter;
  RateLimiter::Args args;

  args.is_dropbox = true;
  args.ignore_guardrails = true;
  args.current_time = base::TimeSeconds(41);

  EXPECT_CALL(limiter, SaveState(_));
  EXPECT_CALL(limiter, LoadState(_));
  ASSERT_TRUE(limiter.ShouldTrace(args));

  EXPECT_CALL(limiter, SaveState(_));
  ASSERT_TRUE(limiter.OnTraceDone(args, true, 42u));

  PerfettoCmdState output{};
  ASSERT_TRUE(limiter.LoadStateConcrete(&output));
  ASSERT_EQ(output.first_trace_timestamp(), 41u);
  ASSERT_EQ(output.last_trace_timestamp(), 41u);
  ASSERT_EQ(output.total_bytes_uploaded(), 42u);
}

TEST(RateLimiterTest, DropBox_EmptyState) {
  StrictMock<MockRateLimiter> limiter;
  RateLimiter::Args args;

  args.is_dropbox = true;
  args.current_time = base::TimeSeconds(10000);

  EXPECT_CALL(limiter, SaveState(_));
  EXPECT_CALL(limiter, LoadState(_));
  ASSERT_TRUE(limiter.ShouldTrace(args));

  EXPECT_CALL(limiter, SaveState(_));
  ASSERT_TRUE(limiter.OnTraceDone(args, true, 1024 * 1024));

  PerfettoCmdState output{};
  ASSERT_TRUE(limiter.LoadStateConcrete(&output));
  EXPECT_EQ(output.total_bytes_uploaded(), 1024u * 1024u);
  EXPECT_EQ(output.first_trace_timestamp(), 10000u);
  EXPECT_EQ(output.last_trace_timestamp(), 10000u);
}

TEST(RateLimiterTest, DropBox_NormalUpload) {
  StrictMock<MockRateLimiter> limiter;
  RateLimiter::Args args;

  PerfettoCmdState input{};
  input.set_first_trace_timestamp(10000);
  input.set_last_trace_timestamp(10000 + 60 * 10);
  input.set_total_bytes_uploaded(1024 * 1024 * 2);
  ASSERT_TRUE(limiter.SaveStateConcrete(input));

  args.is_dropbox = true;
  args.current_time = base::TimeSeconds(input.last_trace_timestamp() + 60 * 10);

  EXPECT_CALL(limiter, LoadState(_));
  ASSERT_TRUE(limiter.ShouldTrace(args));

  EXPECT_CALL(limiter, SaveState(_));
  ASSERT_TRUE(limiter.OnTraceDone(args, true, 1024 * 1024));

  PerfettoCmdState output{};
  ASSERT_TRUE(limiter.LoadStateConcrete(&output));
  EXPECT_EQ(output.total_bytes_uploaded(), 1024u * 1024u * 3);
  EXPECT_EQ(output.first_trace_timestamp(), input.first_trace_timestamp());
  EXPECT_EQ(output.last_trace_timestamp(),
            static_cast<uint64_t>(args.current_time.count()));
}

TEST(RateLimiterTest, DropBox_FailedToLoadState) {
  StrictMock<MockRateLimiter> limiter;
  RateLimiter::Args args;

  args.is_dropbox = true;

  WriteGarbageToFile(limiter.GetStateFilePath().c_str());

  EXPECT_CALL(limiter, LoadState(_));
  EXPECT_CALL(limiter, SaveState(_));
  ASSERT_FALSE(limiter.ShouldTrace(args));

  PerfettoCmdState output{};
  ASSERT_TRUE(limiter.LoadStateConcrete(&output));
  EXPECT_EQ(output.total_bytes_uploaded(), 0u);
  EXPECT_EQ(output.first_trace_timestamp(), 0u);
  EXPECT_EQ(output.last_trace_timestamp(), 0u);
}

TEST(RateLimiterTest, DropBox_NoTimeTravel) {
  StrictMock<MockRateLimiter> limiter;
  RateLimiter::Args args;

  PerfettoCmdState input{};
  input.set_first_trace_timestamp(100);
  input.set_last_trace_timestamp(100);
  ASSERT_TRUE(limiter.SaveStateConcrete(input));

  args.is_dropbox = true;
  args.current_time = base::TimeSeconds(99);

  EXPECT_CALL(limiter, LoadState(_));
  EXPECT_CALL(limiter, SaveState(_));
  ASSERT_FALSE(limiter.ShouldTrace(args));

  PerfettoCmdState output{};
  ASSERT_TRUE(limiter.LoadStateConcrete(&output));
  EXPECT_EQ(output.total_bytes_uploaded(), 0u);
  EXPECT_EQ(output.first_trace_timestamp(), 0u);
  EXPECT_EQ(output.last_trace_timestamp(), 0u);
}

TEST(RateLimiterTest, DropBox_TooSoon) {
  StrictMock<MockRateLimiter> limiter;
  RateLimiter::Args args;

  PerfettoCmdState input{};
  input.set_first_trace_timestamp(10000);
  input.set_last_trace_timestamp(10000);
  ASSERT_TRUE(limiter.SaveStateConcrete(input));

  args.is_dropbox = true;
  args.current_time = base::TimeSeconds(10000 + 60 * 4);

  EXPECT_CALL(limiter, LoadState(_));
  ASSERT_FALSE(limiter.ShouldTrace(args));
}

TEST(RateLimiterTest, DropBox_TooMuch) {
  StrictMock<MockRateLimiter> limiter;
  RateLimiter::Args args;

  PerfettoCmdState input{};
  input.set_total_bytes_uploaded(10 * 1024 * 1024 + 1);
  ASSERT_TRUE(limiter.SaveStateConcrete(input));

  args.is_dropbox = true;
  args.current_time = base::TimeSeconds(60 * 60);

  EXPECT_CALL(limiter, LoadState(_));
  ASSERT_FALSE(limiter.ShouldTrace(args));
}

TEST(RateLimiterTest, DropBox_TooMuch_Override) {
  StrictMock<MockRateLimiter> limiter;
  RateLimiter::Args args;

  PerfettoCmdState input{};
  input.set_total_bytes_uploaded(10 * 1024 * 1024 + 1);
  ASSERT_TRUE(limiter.SaveStateConcrete(input));

  args.is_dropbox = true;
  args.current_time = base::TimeSeconds(60 * 60);
  args.max_upload_bytes_override = 10 * 1024 * 1024 + 2;

  EXPECT_CALL(limiter, LoadState(_));
  ASSERT_TRUE(limiter.ShouldTrace(args));
}

TEST(RateLimiterTest, DropBox_TooMuchWasUploaded) {
  StrictMock<MockRateLimiter> limiter;
  RateLimiter::Args args;

  PerfettoCmdState input{};
  input.set_first_trace_timestamp(1);
  input.set_last_trace_timestamp(1);
  input.set_total_bytes_uploaded(10 * 1024 * 1024 + 1);
  ASSERT_TRUE(limiter.SaveStateConcrete(input));

  args.is_dropbox = true;
  args.current_time = base::TimeSeconds(60 * 60 * 24 + 2);

  EXPECT_CALL(limiter, LoadState(_));
  ASSERT_TRUE(limiter.ShouldTrace(args));

  EXPECT_CALL(limiter, SaveState(_));
  ASSERT_TRUE(limiter.OnTraceDone(args, true, 1024 * 1024));

  PerfettoCmdState output{};
  ASSERT_TRUE(limiter.LoadStateConcrete(&output));
  EXPECT_EQ(output.total_bytes_uploaded(), 1024u * 1024u);
  EXPECT_EQ(output.first_trace_timestamp(),
            static_cast<uint64_t>(args.current_time.count()));
  EXPECT_EQ(output.last_trace_timestamp(),
            static_cast<uint64_t>(args.current_time.count()));
}

TEST(RateLimiterTest, DropBox_FailedToUpload) {
  StrictMock<MockRateLimiter> limiter;
  RateLimiter::Args args;

  args.is_dropbox = true;
  args.current_time = base::TimeSeconds(10000);

  EXPECT_CALL(limiter, SaveState(_));
  EXPECT_CALL(limiter, LoadState(_));
  ASSERT_TRUE(limiter.ShouldTrace(args));
  ASSERT_FALSE(limiter.OnTraceDone(args, false, 1024 * 1024));
}

TEST(RateLimiterTest, DropBox_FailedToSave) {
  StrictMock<MockRateLimiter> limiter;
  RateLimiter::Args args;

  args.is_dropbox = true;
  args.current_time = base::TimeSeconds(10000);

  EXPECT_CALL(limiter, SaveState(_));
  EXPECT_CALL(limiter, LoadState(_));
  ASSERT_TRUE(limiter.ShouldTrace(args));

  EXPECT_CALL(limiter, SaveState(_)).WillOnce(Return(false));
  ASSERT_FALSE(limiter.OnTraceDone(args, true, 1024 * 1024));
}

}  // namespace

}  // namespace perfetto
