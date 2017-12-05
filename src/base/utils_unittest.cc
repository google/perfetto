/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "perfetto/base/utils.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

#include "gtest/gtest.h"

namespace perfetto {
namespace base {
namespace {

TEST(Utils, ArraySize) {
  char char_arr_1[1];
  char char_arr_4[4];
  EXPECT_EQ(1u, ArraySize(char_arr_1));
  EXPECT_EQ(4u, ArraySize(char_arr_4));

  int32_t int32_arr_1[1];
  int32_t int32_arr_4[4];
  EXPECT_EQ(1u, ArraySize(int32_arr_1));
  EXPECT_EQ(4u, ArraySize(int32_arr_4));

  uint64_t int64_arr_1[1];
  uint64_t int64_arr_4[4];
  EXPECT_EQ(1u, ArraySize(int64_arr_1));
  EXPECT_EQ(4u, ArraySize(int64_arr_4));

  char kString[] = "foo";
  EXPECT_EQ(4u, ArraySize(kString));

  struct Bar {
    int32_t a;
    int32_t b;
  };
  Bar bar_1[1];
  Bar bar_4[4];
  EXPECT_EQ(1u, ArraySize(bar_1));
  EXPECT_EQ(4u, ArraySize(bar_4));
}

int pipe_fd[2];

TEST(Utils, EintrWrapper) {
  ASSERT_EQ(0, pipe(pipe_fd));

  struct sigaction sa = {};
  struct sigaction old_sa = {};

// Glibc headers for sa_sigaction trigger this.
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wdisabled-macro-expansion"
#endif
  sa.sa_sigaction = [](int, siginfo_t*, void*) {};
#pragma GCC diagnostic pop

  ASSERT_EQ(0, sigaction(SIGUSR2, &sa, &old_sa));
  int parent_pid = getpid();
  pid_t pid = fork();
  ASSERT_NE(-1, pid);
  if (pid == 0 /* child */) {
    usleep(5000);
    kill(parent_pid, SIGUSR2);
    ignore_result(write(pipe_fd[1], "foo\0", 4));
    _exit(0);
  }

  char buf[6] = {};
  EXPECT_EQ(4, PERFETTO_EINTR(read(pipe_fd[0], buf, sizeof(buf))));
  EXPECT_STREQ("foo", buf);
  EXPECT_EQ(0, PERFETTO_EINTR(close(pipe_fd[0])));
  EXPECT_EQ(0, PERFETTO_EINTR(close(pipe_fd[1])));

  // A 2nd close should fail with the proper errno.
  int res = close(pipe_fd[0]);
  auto err = errno;
  EXPECT_EQ(-1, res);
  EXPECT_EQ(EBADF, err);

  // Restore the old handler.
  sigaction(SIGUSR2, &old_sa, nullptr);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
