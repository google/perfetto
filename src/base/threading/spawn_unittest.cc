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

#include <memory>
#include <optional>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/event_fd.h"
#include "perfetto/ext/base/thread_task_runner.h"
#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/base/threading/poll.h"
#include "perfetto/ext/base/threading/stream.h"
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
  MOCK_METHOD(FuturePollResult<T>, Poll, (PollContext*), (override));
};

template <typename T>
class MockStreamPollable : public StreamPollable<T> {
 public:
  MOCK_METHOD(StreamPollResult<T>, PollNext, (PollContext*), (override));
};

class SpawnUnittest : public testing::Test {
 protected:
  void Drop(base::SpawnHandle) {}
  void Drop(base::Stream<int>) {}

  base::TestTaskRunner task_runner_;

  base::FlatSet<base::PlatformHandle> interested_;
  base::FlatSet<base::PlatformHandle> ready_;
  PollContext ctx_{&interested_, &ready_};

  base::EventFd fd_;
  std::unique_ptr<MockFuturePollable<int>> future_pollable_ =
      std::make_unique<MockFuturePollable<int>>();
  std::unique_ptr<MockStreamPollable<int>> stream_pollable_ =
      std::make_unique<MockStreamPollable<int>>();
};

TEST_F(SpawnUnittest, SpawnFuture) {
  EXPECT_CALL(*future_pollable_, Poll(_))
      .WillOnce([this](PollContext* ctx) {
        fd_.Clear();
        ctx->RegisterInterested(fd_.fd());
        return PendingPollResult();
      })
      .WillOnce(Return(FuturePollResult<int>(1024)));
  auto [handle, future] =
      SpawnResultFuture<int>(&task_runner_, [this]() mutable {
        return base::Future<int>(std::move(future_pollable_));
      });
  base::ignore_result(handle);

  task_runner_.RunUntilIdle();
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());

  task_runner_.RunUntilIdle();
  ASSERT_TRUE(future.Poll(&ctx_).IsPending());

  fd_.Notify();
  task_runner_.RunUntilIdle();

  ASSERT_EQ(future.Poll(&ctx_).item(), 1024);
}

TEST_F(SpawnUnittest, SpawnStream) {
  EXPECT_CALL(*stream_pollable_, PollNext(_))
      .WillOnce([this](PollContext* ctx) {
        fd_.Clear();
        ctx->RegisterInterested(fd_.fd());
        return PendingPollResult();
      })
      .WillOnce(Return(StreamPollResult<int>(1024)))
      .WillOnce([this](PollContext* ctx) {
        fd_.Clear();
        ctx->RegisterInterested(fd_.fd());
        return PendingPollResult();
      })
      .WillOnce(Return(StreamPollResult<int>(2048)))
      .WillOnce(Return(DonePollResult()));
  auto [handle, stream] =
      SpawnResultStream<int>(&task_runner_, [this]() mutable {
        return base::Stream<int>(std::move(stream_pollable_));
      });
  base::ignore_result(handle);

  task_runner_.RunUntilIdle();
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());

  fd_.Notify();
  task_runner_.RunUntilIdle();

  ASSERT_EQ(stream.PollNext(&ctx_).item(), 1024);

  task_runner_.RunUntilIdle();
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());

  fd_.Notify();
  task_runner_.RunUntilIdle();

  ASSERT_EQ(stream.PollNext(&ctx_).item(), 2048);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsDone());
}

TEST_F(SpawnUnittest, SpawnStreamDropStream) {
  EXPECT_CALL(*stream_pollable_, PollNext(_))
      .WillOnce([this](PollContext* ctx) {
        fd_.Clear();
        ctx->RegisterInterested(fd_.fd());
        return PendingPollResult();
      })
      .WillOnce(Return(StreamPollResult<int>(1)))
      .WillOnce(Return(StreamPollResult<int>(2)))
      .WillOnce(Return(StreamPollResult<int>(4)))
      .WillOnce(Return(StreamPollResult<int>(8)))
      .WillOnce(Return(StreamPollResult<int>(16)))
      .WillOnce(Return(StreamPollResult<int>(32)))
      .WillOnce(Return(StreamPollResult<int>(64)))
      .WillOnce(Return(StreamPollResult<int>(128)))
      .WillOnce(Return(StreamPollResult<int>(256)))
      .WillOnce(Return(StreamPollResult<int>(512)))
      .WillOnce(Return(DonePollResult()));

  auto [handle, stream] =
      SpawnResultStream<int>(&task_runner_, [this]() mutable {
        return base::Stream<int>(std::move(stream_pollable_));
      });
  base::ignore_result(handle);

  task_runner_.RunUntilIdle();
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());

  fd_.Notify();
  task_runner_.RunUntilIdle();

  // We should get the first 4 elements and then nothing more: this corresponds
  // to the internal channel buffer size being 4.
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 1);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 2);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 4);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 8);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());

  // Should fill up a bunch more elements.
  task_runner_.RunUntilIdle();

  // Drop the stream.
  Drop(std::move(stream));

  // This should complete the stream.
  task_runner_.RunUntilIdle();

  // Drop the handle and ensure any resulting is completed.
  Drop(std::move(handle));
  task_runner_.RunUntilIdle();
}

TEST_F(SpawnUnittest, SpawnStreamDropHandle) {
  EXPECT_CALL(*stream_pollable_, PollNext(_))
      .WillOnce([this](PollContext* ctx) {
        fd_.Clear();
        ctx->RegisterInterested(fd_.fd());
        return PendingPollResult();
      })
      .WillOnce(Return(StreamPollResult<int>(1)))
      .WillOnce(Return(StreamPollResult<int>(2)))
      .WillOnce(Return(StreamPollResult<int>(4)))
      .WillOnce(Return(StreamPollResult<int>(8)))
      .WillOnce(Return(StreamPollResult<int>(16)))
      .WillOnce(Return(StreamPollResult<int>(32)))
      .WillOnce(Return(StreamPollResult<int>(64)))
      .WillOnce(Return(StreamPollResult<int>(128)))
      .WillOnce(Return(DonePollResult()));

  base::TestTaskRunner task_runner;
  auto [handle, stream] =
      SpawnResultStream<int>(&task_runner, [this]() mutable {
        return base::Stream<int>(std::move(stream_pollable_));
      });
  base::ignore_result(handle);

  task_runner.RunUntilIdle();
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());

  fd_.Notify();
  task_runner.RunUntilIdle();

  // We should get the first 4 elements and then nothing more: this corresponds
  // to the internal channel buffer size being 4.
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 1);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 2);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 4);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 8);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsPending());

  // Should fill up a bunch more elements.
  task_runner.RunUntilIdle();

  // Drop the handle.
  Drop(std::move(handle));

  // We should just get the next four buffered elements and the stream should
  // complete.
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 16);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 32);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 64);
  ASSERT_EQ(stream.PollNext(&ctx_).item(), 128);
  ASSERT_TRUE(stream.PollNext(&ctx_).IsDone());
}

}  // namespace
}  // namespace base
}  // namespace perfetto
