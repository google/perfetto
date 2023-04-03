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

#include "perfetto/ext/base/threading/stream.h"

#include <vector>

#include "perfetto/base/platform_handle.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/event_fd.h"
#include "perfetto/ext/base/threading/future_combinators.h"
#include "perfetto/ext/base/threading/poll.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

using testing::_;
using testing::ElementsAre;
using testing::Return;
using testing::UnorderedElementsAre;

template <typename T>
class MockPollable : public FuturePollable<T> {
 public:
  MOCK_METHOD(FuturePollResult<T>, Poll, (PollContext*), (override));
};

template <typename T>
class MockStreamPollable : public StreamPollable<T> {
 public:
  MOCK_METHOD(StreamPollResult<T>, PollNext, (PollContext*), (override));
};

class StreamUnittest : public ::testing::Test {
 protected:
  base::FlatSet<base::PlatformHandle> interested_;
  base::FlatSet<base::PlatformHandle> ready_;
  PollContext ctx_{&interested_, &ready_};
};

TEST_F(StreamUnittest, PollableImmediateResult) {
  std::unique_ptr<MockStreamPollable<int>> int_pollable(
      new MockStreamPollable<int>());
  EXPECT_CALL(*int_pollable, PollNext(_))
      .WillOnce(Return(StreamPollResult<int>(0)));

  base::Stream<int> stream(std::move(int_pollable));
  auto res = stream.PollNext(&ctx_);
  ASSERT_FALSE(res.IsPending());
  ASSERT_EQ(res.item(), 0);
}

TEST_F(StreamUnittest, PollablePendingThenResult) {
  std::unique_ptr<MockStreamPollable<int>> int_pollable(
      new MockStreamPollable<int>());
  EXPECT_CALL(*int_pollable, PollNext(_))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<int>(1)))
      .WillOnce(Return(DonePollResult()));

  base::Stream<int> stream(std::move(int_pollable));
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 1);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsDone());
}

TEST_F(StreamUnittest, Map) {
  std::unique_ptr<MockStreamPollable<int>> int_pollable(
      new MockStreamPollable<int>());
  EXPECT_CALL(*int_pollable, PollNext(_))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<int>(1)))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<int>(2)))
      .WillOnce(Return(DonePollResult()));

  auto stream = base::Stream<int>(std::move(int_pollable))
                    .MapFuture([](int res) -> base::Future<std::string> {
                      return std::to_string(res);
                    });
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());
  ASSERT_EQ(stream.PollNext(&ctx_).item(), "1");
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());
  ASSERT_EQ(stream.PollNext(&ctx_).item(), "2");
  ASSERT_TRUE(stream.PollNext(&ctx_).IsDone());
}

TEST_F(StreamUnittest, Concat) {
  std::unique_ptr<MockStreamPollable<int>> int_pollable(
      new MockStreamPollable<int>());
  EXPECT_CALL(*int_pollable, PollNext(_))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<int>(1)))
      .WillOnce(Return(StreamPollResult<int>(2)))
      .WillOnce(Return(DonePollResult()));

  std::unique_ptr<MockStreamPollable<int>> concat_pollable(
      new MockStreamPollable<int>());
  EXPECT_CALL(*concat_pollable, PollNext(_))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<int>(3)))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<int>(4)))
      .WillOnce(Return(DonePollResult()));

  auto stream = base::Stream<int>(std::move(int_pollable))
                    .Concat(base::Stream<int>(std::move(concat_pollable)));
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 1);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 2);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 3);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 4);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsDone());
}

TEST_F(StreamUnittest, AllOkCollectorEarly) {
  std::unique_ptr<MockStreamPollable<base::Status>> pollable(
      new MockStreamPollable<base::Status>());
  EXPECT_CALL(*pollable, PollNext(_))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<base::Status>(base::OkStatus())))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<base::Status>(base::ErrStatus("Bad"))));

  auto future = base::Stream<base::Status>(std::move(pollable))
                    .Collect(base::AllOkCollector());
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());
  ASSERT_FALSE(future.Poll(&ctx_).item().ok());
}

TEST_F(StreamUnittest, AllOkCollectorComplete) {
  std::unique_ptr<MockStreamPollable<base::Status>> pollable(
      new MockStreamPollable<base::Status>());
  EXPECT_CALL(*pollable, PollNext(_))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<base::Status>(base::OkStatus())))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<base::Status>(base::OkStatus())))
      .WillOnce(Return(StreamPollResult<base::Status>(base::OkStatus())))
      .WillOnce(Return(DonePollResult()));

  auto future = base::Stream<base::Status>(std::move(pollable))
                    .Collect(base::AllOkCollector());
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());
  ASSERT_TRUE(future.Poll(&ctx_).item().ok());
}

TEST_F(StreamUnittest, ToFutureCheckedCollector) {
  std::unique_ptr<MockStreamPollable<base::Status>> pollable(
      new MockStreamPollable<base::Status>());
  EXPECT_CALL(*pollable, PollNext(_))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<base::Status>(base::OkStatus())))
      .WillOnce(Return(DonePollResult()));

  auto future = base::Stream<base::Status>(std::move(pollable))
                    .Collect(base::ToFutureCheckedCollector<base::Status>());
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());
  ASSERT_TRUE(future.Poll(&ctx_).item().ok());
}

TEST_F(StreamUnittest, StatusOrCollectorEarly) {
  std::unique_ptr<MockStreamPollable<base::StatusOr<int>>> pollable(
      new MockStreamPollable<base::StatusOr<int>>());
  EXPECT_CALL(*pollable, PollNext(_))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<base::StatusOr<int>>(1024)))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(
          StreamPollResult<base::StatusOr<int>>(base::ErrStatus("Bad"))));

  auto future = base::Stream<base::StatusOr<int>>(std::move(pollable))
                    .Collect(base::StatusOrVectorCollector<int>());
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());
  ASSERT_FALSE(future.Poll(&ctx_).item().ok());
}

TEST_F(StreamUnittest, StatusOrCollectorComplete) {
  std::unique_ptr<MockStreamPollable<base::StatusOr<int>>> pollable(
      new MockStreamPollable<base::StatusOr<int>>());
  EXPECT_CALL(*pollable, PollNext(_))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<base::StatusOr<int>>(1024)))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(StreamPollResult<base::StatusOr<int>>(2048)))
      .WillOnce(Return(DonePollResult()));

  auto future = base::Stream<base::StatusOr<int>>(std::move(pollable))
                    .Collect(base::StatusOrVectorCollector<int>());
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());
  ASSERT_THAT(future.Poll(&ctx_).item().value(), ElementsAre(1024, 2048));
}

TEST_F(StreamUnittest, StreamFrom) {
  auto stream = base::StreamFrom(std::vector<int>({1, 2, 4}));

  ASSERT_EQ(stream.PollNext(&ctx_).item(), 1);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 2);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 4);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsDone());
}

TEST_F(StreamUnittest, EmptyStream) {
  auto stream = base::EmptyStream<int>();
  ASSERT_TRUE(stream.PollNext(&ctx_).IsDone());
}

TEST_F(StreamUnittest, StreamOf) {
  auto stream = base::StreamOf(1, 2);

  ASSERT_EQ(stream.PollNext(&ctx_).item(), 1);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 2);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsDone());
}

TEST_F(StreamUnittest, StreamFromFuture) {
  std::unique_ptr<MockPollable<int>> int_pollable(new MockPollable<int>());
  EXPECT_CALL(*int_pollable, Poll(_))
      .WillOnce(Return(PendingPollResult()))
      .WillOnce(Return(FuturePollResult<int>(1)));

  auto stream =
      base::StreamFromFuture(base::Future<int>(std::move(int_pollable)));

  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 1);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsDone());
}

TEST_F(StreamUnittest, OnDestroyStream) {
  bool destroyed = false;
  {
    auto stream =
        base::OnDestroyStream<int>([&destroyed]() { destroyed = true; });
    ASSERT_FALSE(destroyed);
    ASSERT_TRUE(stream.PollNext(&ctx_).IsDone());
    ASSERT_FALSE(destroyed);
  }
  ASSERT_TRUE(destroyed);
}

TEST_F(StreamUnittest, FlattenStreams) {
  EventFd event_fd1, event_fd2, event_fd3, event_fd4;
  const PlatformHandle fd1 = event_fd1.fd(), fd2 = event_fd2.fd(),
                       fd3 = event_fd3.fd(), fd4 = event_fd4.fd();
  std::unique_ptr<MockStreamPollable<int>> a(new MockStreamPollable<int>());
  EXPECT_CALL(*a, PollNext(_))
      .WillOnce([fd1](PollContext* ctx) {
        ctx->RegisterInterested(fd1);
        return PendingPollResult();
      })
      .WillOnce(Return(StreamPollResult<int>(1)))
      .WillOnce(Return(DonePollResult()));

  std::unique_ptr<MockStreamPollable<int>> b(new MockStreamPollable<int>());
  EXPECT_CALL(*b, PollNext(_))
      .WillOnce([fd2](PollContext* ctx) {
        ctx->RegisterInterested(fd2);
        return PendingPollResult();
      })
      .WillOnce([fd2](PollContext* ctx) {
        ctx->RegisterInterested(fd2);
        return PendingPollResult();
      })
      .WillOnce(Return(StreamPollResult<int>(2)))
      .WillOnce(Return(DonePollResult()));

  std::unique_ptr<MockStreamPollable<int>> c(new MockStreamPollable<int>());
  EXPECT_CALL(*c, PollNext(_))
      .WillOnce(Return(StreamPollResult<int>(3)))
      .WillOnce([fd3, fd4](PollContext* ctx) {
        ctx->RegisterInterested(fd3);
        ctx->RegisterInterested(fd4);
        return PendingPollResult();
      })
      .WillOnce(Return(DonePollResult()));

  std::vector<Stream<int>> streams;
  streams.emplace_back(std::move(a));
  streams.emplace_back(std::move(b));
  streams.emplace_back(std::move(c));

  auto stream = base::FlattenStreams(std::move(streams));
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 3);
  ASSERT_THAT(interested_, ElementsAre());

  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());
  ASSERT_THAT(interested_, UnorderedElementsAre(fd1, fd2, fd3, fd4));

  interested_.clear();
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());
  ASSERT_THAT(interested_, UnorderedElementsAre(fd1, fd2, fd3, fd4));

  interested_.clear();
  ready_ = {fd1};
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 1);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());
  ASSERT_THAT(interested_, UnorderedElementsAre(fd2, fd3, fd4));

  interested_.clear();
  ready_ = {};
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());
  ASSERT_THAT(interested_, ElementsAre(fd2, fd3, fd4));

  interested_.clear();
  ready_ = {fd1, fd2, fd3};
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 2);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsDone());
}

}  // namespace
}  // namespace base
}  // namespace perfetto
