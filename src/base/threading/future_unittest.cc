/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "perfetto/ext/base/threading/future.h"

#include <memory>

#include "perfetto/base/flat_set.h"
#include "perfetto/base/platform_handle.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

using testing::_;
using testing::Return;

template <typename T>
class MockPollable : public FuturePollable<T> {
 public:
  MOCK_METHOD(FuturePollResult<T>, Poll, (PollContext*), (override));
};

class FutureUnittest : public ::testing::Test {
 public:
  base::FlatSet<base::PlatformHandle> interested_;
  base::FlatSet<base::PlatformHandle> ready_;
  PollContext ctx_{&interested_, &ready_};
};

TEST_F(FutureUnittest, PollableImmediateResult) {
  std::unique_ptr<MockPollable<int>> int_pollable(new MockPollable<int>());
  EXPECT_CALL(*int_pollable, Poll(_))
      .WillOnce(Return(FuturePollResult<int>(0)));

  base::Future<int> future(std::move(int_pollable));
  auto res = future.Poll(&ctx_);
  ASSERT_FALSE(res.IsPending());
  ASSERT_EQ(res.item(), 0);
}

TEST_F(FutureUnittest, PollablePendingThenResult) {
  std::unique_ptr<MockPollable<int>> int_pollable(new MockPollable<int>());
  EXPECT_CALL(*int_pollable, Poll(_))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(FuturePollResult<int>(1)));

  base::Future<int> future(std::move(int_pollable));
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());
  ASSERT_EQ(future.Poll(&ctx_).item(), 1);
}

TEST_F(FutureUnittest, ImmediateFuture) {
  base::Future<int> future(100);
  ASSERT_EQ(future.Poll(&ctx_).item(), 100);
}

TEST_F(FutureUnittest, ContinueWithBothImmediate) {
  auto future = base::Future<int>(100).ContinueWith(
      [](int res) -> Future<int> { return res * 2; });
  ASSERT_EQ(future.Poll(&ctx_).item(), 200);
}

TEST_F(FutureUnittest, ImmediateContinueWithPending) {
  auto future = base::Future<int>(100).ContinueWith([](int res) {
    std::unique_ptr<MockPollable<int>> pollable(new MockPollable<int>());
    EXPECT_CALL(*pollable, Poll(_))
        .WillOnce(Return(PendingPollResult()))
        .WillOnce(Return(FuturePollResult<int>(res * 2)));
    return Future<int>(std::move(pollable));
  });
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());
  ASSERT_EQ(future.Poll(&ctx_).item(), 200);
}

TEST_F(FutureUnittest, PendingContinueWithImmediate) {
  std::unique_ptr<MockPollable<int>> pollable(new MockPollable<int>());
  EXPECT_CALL(*pollable, Poll(_))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(FuturePollResult<int>(100)));
  auto future =
      base::Future<int>(std::move(pollable))
          .ContinueWith([](int res) -> Future<int> { return res * 2; });
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());
  ASSERT_EQ(future.Poll(&ctx_).item(), 200);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
