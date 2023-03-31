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

#include "perfetto/ext/base/threading/util.h"

#include <optional>

#include "perfetto/base/flat_set.h"
#include "perfetto/base/platform_handle.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/event_fd.h"
#include "perfetto/ext/base/threading/channel.h"
#include "perfetto/ext/base/threading/poll.h"
#include "perfetto/ext/base/threading/stream.h"
#include "perfetto/ext/base/threading/thread_pool.h"
#include "perfetto/ext/base/unix_task_runner.h"
#include "perfetto/ext/base/waitable_event.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

int WaitForFutureReady(base::Future<int>& stream,
                       base::FlatSet<base::PlatformHandle>& interested,
                       PollContext& ctx) {
  auto res = stream.Poll(&ctx);
  for (; res.IsPending(); res = stream.Poll(&ctx)) {
    PERFETTO_CHECK(interested.size() == 1);
    base::BlockUntilReadableFd(*interested.begin());
    interested = {};
  }
  return res.item();
}

std::optional<int> WaitForStreamReady(
    base::Stream<int>& stream,
    base::FlatSet<base::PlatformHandle>& interested,
    PollContext& ctx) {
  auto res = stream.PollNext(&ctx);
  for (; res.IsPending(); res = stream.PollNext(&ctx)) {
    PERFETTO_CHECK(interested.size() == 1);
    base::BlockUntilReadableFd(*interested.begin());
    interested = {};
  }
  return res.IsDone() ? std::nullopt : std::make_optional(res.item());
}

TEST(UtilUnittest, BlockUntilReadableFd) {
  base::WaitableEvent evt;
  base::EventFd main_to_background;
  base::EventFd background_to_main;
  std::thread thread([&main_to_background, &background_to_main] {
    base::BlockUntilReadableFd(main_to_background.fd());
    background_to_main.Notify();
  });
  main_to_background.Notify();
  base::BlockUntilReadableFd(background_to_main.fd());
  thread.join();
}

TEST(UtilUnittest, ReadChannelStream) {
  base::Channel<int> channel(1);
  auto stream = base::ReadChannelStream(&channel);

  base::FlatSet<base::PlatformHandle> interested;
  base::FlatSet<base::PlatformHandle> ready;
  PollContext ctx(&interested, &ready);

  ASSERT_TRUE(stream.PollNext(&ctx).IsPending());
  ASSERT_EQ(interested.count(channel.read_fd()), 1u);
  interested = {};

  ASSERT_TRUE(channel.WriteNonBlocking(1).success);
  ASSERT_EQ(stream.PollNext(&ctx).item(), 1);

  ASSERT_TRUE(stream.PollNext(&ctx).IsPending());
  ASSERT_EQ(interested.count(channel.read_fd()), 1u);
  interested = {};

  ASSERT_TRUE(channel.WriteNonBlocking(2).success);
  channel.Close();

  ASSERT_EQ(stream.PollNext(&ctx).item(), 2);
  ASSERT_TRUE(stream.PollNext(&ctx).IsDone());
}

TEST(UtilUnittest, WriteChannelFuture) {
  base::Channel<int> channel(1);

  base::FlatSet<base::PlatformHandle> interested;
  base::FlatSet<base::PlatformHandle> ready;
  PollContext ctx(&interested, &ready);

  ASSERT_TRUE(channel.WriteNonBlocking(1).success);
  ASSERT_FALSE(channel.WriteNonBlocking(2).success);

  auto future = base::WriteChannelFuture(&channel, 3);
  ASSERT_TRUE(future.Poll(&ctx).IsPending());
  ASSERT_EQ(interested.count(channel.write_fd()), 1u);
  interested = {};

  ASSERT_EQ(channel.ReadNonBlocking().item, 1);
  ASSERT_EQ(channel.ReadNonBlocking().item, std::nullopt);

  ASSERT_FALSE(future.Poll(&ctx).IsPending());
  ASSERT_EQ(channel.ReadNonBlocking().item, 3);
}

TEST(UtilUnittest, RunOnThreadPool) {
  base::FlatSet<base::PlatformHandle> interested;
  base::FlatSet<base::PlatformHandle> ready;
  PollContext ctx(&interested, &ready);

  base::ThreadPool pool(1);
  base::Stream<int> stream =
      base::RunOnThreadPool<int>(&pool, [counter = 0]() mutable {
        return counter == 2 ? std::nullopt : std::make_optional(counter++);
      });
  ASSERT_EQ(WaitForStreamReady(stream, interested, ctx), 0);
  ASSERT_EQ(WaitForStreamReady(stream, interested, ctx), 1);
  ASSERT_EQ(WaitForStreamReady(stream, interested, ctx), std::nullopt);
}

TEST(UtilUnittest, RunOnceOnThreadPool) {
  base::FlatSet<base::PlatformHandle> interested;
  base::FlatSet<base::PlatformHandle> ready;
  PollContext ctx(&interested, &ready);

  base::ThreadPool pool(1);
  base::Future<int> fut =
      base::RunOnceOnThreadPool<int>(&pool, []() mutable { return 1; });
  ASSERT_EQ(WaitForFutureReady(fut, interested, ctx), 1);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
