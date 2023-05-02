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

#include "perfetto/ext/base/threading/channel.h"

#include <array>
#include <memory>
#include <optional>

#include "perfetto/base/platform_handle.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/utils.h"
#include "test/gtest_and_gmock.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <Windows.h>
#include <synchapi.h>
#else
#include <poll.h>
#endif

namespace perfetto {
namespace base {
namespace {

using ReadResult = Channel<int>::ReadResult;
using WriteResult = Channel<int>::WriteResult;

bool IsReady(base::PlatformHandle fd) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  std::array<base::PlatformHandle, 1> poll_fds{fd};
  DWORD ret =
      WaitForMultipleObjects(static_cast<DWORD>(poll_fds.size()), &poll_fds[0],
                             /*bWaitAll=*/false, 0);
  PERFETTO_CHECK(ret == WAIT_TIMEOUT || ret == 0);
  return ret == 0;
#else
  std::array<struct pollfd, 1> poll_fds;
  poll_fds[0].fd = fd;
  poll_fds[0].events = POLLIN | POLLHUP;
  poll_fds[0].revents = 0;

  int ret = PERFETTO_EINTR(
      poll(&poll_fds[0], static_cast<nfds_t>(poll_fds.size()), 0));
  PERFETTO_CHECK(ret == 0 || ret == 1);
  return ret == 1;
#endif
}

TEST(ChannelUnittest, SingleElementBuffer) {
  Channel<int> channel(1);
  ASSERT_TRUE(IsReady(channel.write_fd()));
  ASSERT_FALSE(IsReady(channel.read_fd()));

  ASSERT_EQ(channel.WriteNonBlocking(100), WriteResult(true, false));
  ASSERT_EQ(channel.WriteNonBlocking(101), WriteResult(false, false));

  ASSERT_FALSE(IsReady(channel.write_fd()));
  ASSERT_TRUE(IsReady(channel.read_fd()));

  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(100, false));
  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(std::nullopt, false));

  ASSERT_TRUE(IsReady(channel.write_fd()));
  ASSERT_FALSE(IsReady(channel.read_fd()));
}

TEST(ChannelUnittest, MultiElementBuffer) {
  Channel<int> channel(2);
  ASSERT_TRUE(IsReady(channel.write_fd()));
  ASSERT_FALSE(IsReady(channel.read_fd()));

  ASSERT_EQ(channel.WriteNonBlocking(100), WriteResult(true, false));
  ASSERT_TRUE(IsReady(channel.write_fd()));
  ASSERT_TRUE(IsReady(channel.read_fd()));

  ASSERT_EQ(channel.WriteNonBlocking(101), WriteResult(true, false));
  ASSERT_FALSE(IsReady(channel.write_fd()));
  ASSERT_TRUE(IsReady(channel.read_fd()));

  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(100, false));
  ASSERT_TRUE(IsReady(channel.write_fd()));
  ASSERT_TRUE(IsReady(channel.read_fd()));

  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(101, false));
  ASSERT_TRUE(IsReady(channel.write_fd()));
  ASSERT_FALSE(IsReady(channel.read_fd()));

  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(std::nullopt, false));
  ASSERT_TRUE(IsReady(channel.write_fd()));
  ASSERT_FALSE(IsReady(channel.read_fd()));
}

TEST(ChannelUnittest, CloseEmptyChannel) {
  Channel<int> channel(1);

  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(std::nullopt, false));
  ASSERT_FALSE(IsReady(channel.read_fd()));

  channel.Close();

  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(std::nullopt, true));
  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(std::nullopt, true));

  ASSERT_TRUE(IsReady(channel.read_fd()));
  ASSERT_TRUE(IsReady(channel.read_fd()));
}

TEST(ChannelUnittest, WriteDoesNotMoveIfFalse) {
  Channel<std::unique_ptr<int>> channel(1);

  std::unique_ptr<int> first(new int(100));
  int* first_ptr = first.get();
  ASSERT_EQ(channel.WriteNonBlocking(std::move(first)),
            Channel<std::unique_ptr<int>>::WriteResult(true, false));
  ASSERT_EQ(first.get(), nullptr);

  std::unique_ptr<int> second(new int(101));
  ASSERT_EQ(channel.WriteNonBlocking(std::move(second)),
            Channel<std::unique_ptr<int>>::WriteResult(false, false));
  ASSERT_NE(second.get(), nullptr);
  ASSERT_EQ(*second, 101);

  auto res = channel.ReadNonBlocking();
  ASSERT_EQ(res.item->get(), first_ptr);
}

TEST(ChannelUnittest, ReadAfterClose) {
  Channel<int> channel(1);
  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(std::nullopt, false));
  ASSERT_EQ(channel.WriteNonBlocking(100), WriteResult(true, false));
  channel.Close();

  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(100, true));
  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(std::nullopt, true));
}

TEST(ChannelUnittest, WriteAfterClose) {
  Channel<int> channel(1);
  ASSERT_EQ(channel.WriteNonBlocking(100), WriteResult(true, false));
  ASSERT_EQ(channel.WriteNonBlocking(101), WriteResult(false, false));
  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(100, false));
  channel.Close();

  ASSERT_EQ(channel.WriteNonBlocking(101), WriteResult(false, true));
}

TEST(ChannelUnittest, EmptyClosedChannel) {
  Channel<int> channel(1);
  ASSERT_FALSE(IsReady(channel.read_fd()));
  ASSERT_TRUE(IsReady(channel.write_fd()));
  channel.Close();
  ASSERT_TRUE(IsReady(channel.write_fd()));
  ASSERT_TRUE(IsReady(channel.write_fd()));
  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(std::nullopt, true));
  ASSERT_TRUE(IsReady(channel.write_fd()));
  ASSERT_TRUE(IsReady(channel.read_fd()));
}

TEST(ChannelUnittest, FullClosedChannel) {
  Channel<int> channel(1);
  ASSERT_FALSE(IsReady(channel.read_fd()));
  ASSERT_EQ(channel.WriteNonBlocking(100), WriteResult(true, false));
  ASSERT_TRUE(IsReady(channel.read_fd()));
  ASSERT_FALSE(IsReady(channel.write_fd()));
  channel.Close();
  ASSERT_TRUE(IsReady(channel.write_fd()));

  ASSERT_EQ(channel.ReadNonBlocking(), ReadResult(100, true));
  ASSERT_TRUE(IsReady(channel.write_fd()));
  ASSERT_TRUE(IsReady(channel.read_fd()));
}

}  // namespace
}  // namespace base
}  // namespace perfetto
