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

#include "perfetto/ext/base/threading/spawn.h"

#include "perfetto/ext/base/event_fd.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/thread_task_runner.h"
#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/base/threading/poll.h"
#include "perfetto/ext/base/threading/util.h"
#include "perfetto/ext/base/unix_task_runner.h"
#include "src/base/test/test_task_runner.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

using ::testing::_;
using ::testing::Return;

template <typename T>
class MockFuturePollable : public FuturePollable<T> {
 public:
  MOCK_METHOD1(Poll, FuturePollResult<T>(PollContext*));
};

template <typename T>
class MockStreamPollable : public StreamPollable<T> {
 public:
  MOCK_METHOD1(PollNext, StreamPollResult<T>(PollContext*));
};

TEST(SpawnUnittest, SpawnFuture) {
  base::TestTaskRunner task_runner;

  base::EventFd fd;
  auto pollable = std::make_unique<MockFuturePollable<int>>();
  EXPECT_CALL(*pollable, Poll(_))
      .WillOnce([&fd](PollContext* ctx) {
        fd.Clear();
        ctx->RegisterInterested(fd.fd());
        return PendingPollResult();
      })
      .WillOnce(Return(FuturePollResult<int>(1024)));
  auto res = SpawnResultFuture<int>(
      &task_runner,
      [pollable = std::make_shared<std::unique_ptr<MockFuturePollable<int>>>(
           std::move(pollable))]() mutable {
        return base::Future<int>(std::move(*pollable));
      });

  task_runner.RunUntilIdle();
  ASSERT_EQ(res.channel()->ReadNonBlocking().item, base::nullopt);

  task_runner.RunUntilIdle();
  ASSERT_EQ(res.channel()->ReadNonBlocking().item, base::nullopt);

  fd.Notify();
  task_runner.RunUntilIdle();

  auto read = res.channel()->ReadNonBlocking();
  ASSERT_EQ(read.item, 1024);
  ASSERT_TRUE(read.is_closed);

  read = res.channel()->ReadNonBlocking();
  ASSERT_TRUE(read.is_closed);
}

TEST(SpawnUnittest, SpawnStream) {
  base::TestTaskRunner task_runner;

  base::EventFd fd;
  auto pollable = std::make_unique<MockStreamPollable<int>>();
  EXPECT_CALL(*pollable, PollNext(_))
      .WillOnce([&fd](PollContext* ctx) {
        fd.Clear();
        ctx->RegisterInterested(fd.fd());
        return PendingPollResult();
      })
      .WillOnce(Return(StreamPollResult<int>(1024)))
      .WillOnce([&fd](PollContext* ctx) {
        fd.Clear();
        ctx->RegisterInterested(fd.fd());
        return PendingPollResult();
      })
      .WillOnce(Return(StreamPollResult<int>(2048)))
      .WillOnce(Return(DonePollResult()));
  auto res = SpawnResultStream<int>(
      &task_runner,
      [pollable = std::make_shared<std::unique_ptr<MockStreamPollable<int>>>(
           std::move(pollable))]() mutable {
        return base::Stream<int>(std::move(*pollable));
      });

  task_runner.RunUntilIdle();
  ASSERT_EQ(res.channel()->ReadNonBlocking().item, base::nullopt);

  fd.Notify();
  task_runner.RunUntilIdle();

  auto read = res.channel()->ReadNonBlocking();
  ASSERT_EQ(read.item, 1024);
  ASSERT_FALSE(read.is_closed);

  task_runner.RunUntilIdle();
  ASSERT_EQ(res.channel()->ReadNonBlocking().item, base::nullopt);

  fd.Notify();
  task_runner.RunUntilIdle();

  read = res.channel()->ReadNonBlocking();
  ASSERT_EQ(read.item, 2048);
  ASSERT_TRUE(read.is_closed);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
