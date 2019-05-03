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

#include "src/profiling/memory/proc_utils.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace profiling {
namespace {

using ::testing::Contains;
using ::testing::Not;

TEST(ProcUtilsTest, NormalizeNoop) {
  char kCmdline[] = "surfaceflinger\0";
  std::string name;
  ASSERT_TRUE(NormalizeCmdLine(kCmdline, sizeof(kCmdline), &name));
  EXPECT_EQ(name, "surfaceflinger");
}

TEST(ProcUtilsTest, NormalizePath) {
  char kCmdline[] = "/system/bin/surfaceflinger\0";
  std::string name;
  ASSERT_TRUE(NormalizeCmdLine(kCmdline, sizeof(kCmdline), &name));
  EXPECT_EQ(name, "surfaceflinger");
}

TEST(ProcUtilsTest, NormalizeAt) {
  char kCmdline[] = "some.app@2.0\0";
  std::string name;
  ASSERT_TRUE(NormalizeCmdLine(kCmdline, sizeof(kCmdline), &name));
  EXPECT_EQ(name, "some.app");
}

TEST(ProcUtilsTest, FindProfilablePids) {
  std::set<pid_t> pids;
  int pipefds[2];
  PERFETTO_CHECK(pipe(pipefds) == 0);
  pid_t pid = fork();
  PERFETTO_CHECK(pid >= 0);
  switch (pid) {
    case 0: {
      close(pipefds[1]);
      char buf[1];
      // Block until the other end shuts down the pipe.
      read(pipefds[0], buf, sizeof(buf));
      exit(0);
    }
    default:
      close(pipefds[0]);
      break;
  }
  FindAllProfilablePids(&pids);
  close(pipefds[1]);
  EXPECT_THAT(pids, Contains(pid));
  EXPECT_THAT(pids, Not(Contains(getpid())));
  PERFETTO_CHECK(PERFETTO_EINTR(waitpid(pid, nullptr, 0)) == pid);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
