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

#include "src/profiling/memory/client.h"

#include <signal.h>

#include <thread>

#include "perfetto/base/thread_utils.h"
#include "perfetto/ext/base/unix_socket.h"
#include "src/profiling/memory/wire_protocol.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

TEST(ClientTest, GetThreadStackRangeBase) {
  std::thread th([] {
    StackRange stackrange = GetThreadStackRange();
    ASSERT_NE(stackrange.begin, nullptr);
    ASSERT_NE(stackrange.end, nullptr);
    // The implementation assumes the stack grows from higher addresses to
    // lower. We will need to rework once we encounter architectures where the
    // stack grows the other way.
    EXPECT_LT(stackrange.begin, __builtin_frame_address(0));
    EXPECT_GT(stackrange.end, __builtin_frame_address(0));
  });
  th.join();
}

#if defined(ADDRESS_SANITIZER)
#define MAYBE_GetSigaltStackRange DISABLED_GetSigaltStackRange
#else
#define MAYBE_GetSigaltStackRange GetSigaltStackRange
#endif

TEST(ClientTest, MAYBE_GetSigaltStackRange) {
  char stack[4096];
  stack_t altstack{};
  stack_t old_altstack{};
  altstack.ss_sp = stack;
  altstack.ss_size = sizeof(stack);
  ASSERT_NE(sigaltstack(&altstack, &old_altstack), -1);

  struct sigaction oldact;
  struct sigaction newact {};

  static StackRange stackrange;
  static const char* stackptr;
  newact.sa_handler = [](int) {
    stackrange = GetSigAltStackRange();
    stackptr = static_cast<char*>(__builtin_frame_address(0));
  };
  newact.sa_flags = SA_ONSTACK;
  int res = sigaction(SIGUSR1, &newact, &oldact);
  ASSERT_NE(res, -1);

  raise(SIGUSR1);

  PERFETTO_CHECK(sigaction(SIGUSR1, &oldact, nullptr) != -1);
  PERFETTO_CHECK(sigaltstack(&old_altstack, nullptr) != -1);

  ASSERT_EQ(stackrange.begin, stack);
  ASSERT_EQ(stackrange.end, &stack[4096]);
  ASSERT_LT(stackrange.begin, stackptr);
  ASSERT_GT(stackrange.end, stackptr);
}

TEST(ClientTest, GetMainThreadStackRange) {
  if (getpid() != base::GetThreadId())
    GTEST_SKIP() << "This test has to run on the main thread.";

  StackRange stackrange = GetMainThreadStackRange();
  ASSERT_NE(stackrange.begin, nullptr);
  ASSERT_NE(stackrange.end, nullptr);
  // The implementation assumes the stack grows from higher addresses to
  // lower. We will need to rework once we encounter architectures where the
  // stack grows the other way.
  EXPECT_LT(stackrange.begin, __builtin_frame_address(0));
  EXPECT_GT(stackrange.end, __builtin_frame_address(0));
}

TEST(ClientTest, IsMainThread) {
  // Our code relies on the fact that getpid() == GetThreadId() if this
  // process/thread is the main thread of the process. This test ensures that is
  // true.
  auto pid = getpid();
  auto main_thread_id = base::GetThreadId();
  EXPECT_EQ(pid, main_thread_id);
  std::thread th(
      [main_thread_id] { EXPECT_NE(main_thread_id, base::GetThreadId()); });
  th.join();
}

TEST(ClientTest, GetMaxTriesBlock) {
  ClientConfiguration cfg = {};
  cfg.block_client = true;
  cfg.block_client_timeout_us = 200;
  EXPECT_EQ(GetMaxTries(cfg), 2u);
}

TEST(ClientTest, GetMaxTriesBlockSmall) {
  ClientConfiguration cfg = {};
  cfg.block_client = true;
  cfg.block_client_timeout_us = 99;
  EXPECT_EQ(GetMaxTries(cfg), 1u);
}

TEST(ClientTest, GetMaxTriesBlockVerySmall) {
  ClientConfiguration cfg = {};
  cfg.block_client = true;
  cfg.block_client_timeout_us = 1;
  EXPECT_EQ(GetMaxTries(cfg), 1u);
}

TEST(ClientTest, GetMaxTriesBlockInfinite) {
  ClientConfiguration cfg = {};
  cfg.block_client = true;
  cfg.block_client_timeout_us = 0;
  EXPECT_EQ(GetMaxTries(cfg), kInfiniteTries);
}

TEST(ClientTest, GetMaxTriesNoBlock) {
  ClientConfiguration cfg = {};
  cfg.block_client = false;
  cfg.block_client_timeout_us = 200;
  EXPECT_EQ(GetMaxTries(cfg), 1u);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
