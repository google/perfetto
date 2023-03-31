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

#include "src/profiling/common/proc_utils.h"

#include <optional>

#include "perfetto/ext/base/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

using ::testing::Contains;
using ::testing::Not;

std::string NormalizeToString(char* cmdline, size_t size) {
  ssize_t new_size = NormalizeCmdLine(&cmdline, size);
  if (new_size == -1)
    return "";
  return std::string(cmdline, static_cast<size_t>(new_size));
}

TEST(ProcUtilsTest, NormalizeNoop) {
  char kCmdline[] = "surfaceflinger";
  EXPECT_EQ(NormalizeToString(kCmdline, sizeof(kCmdline)), "surfaceflinger");
}

TEST(ProcUtilsTest, NormalizeTwoArgs) {
  char kCmdline[] = "surfaceflinger\0--foo";
  EXPECT_EQ(NormalizeToString(kCmdline, sizeof(kCmdline)), "surfaceflinger");
}

TEST(ProcUtilsTest, NormalizePath) {
  char kCmdline[] = "/system/bin/surfaceflinger";
  EXPECT_EQ(NormalizeToString(kCmdline, sizeof(kCmdline)), "surfaceflinger");
}

TEST(ProcUtilsTest, NormalizeAt) {
  char kCmdline[] = "some.app@2.0";
  EXPECT_EQ(NormalizeToString(kCmdline, sizeof(kCmdline)), "some.app");
}

TEST(ProcUtilsTest, NormalizeEmpty) {
  char kCmdline[] = "";
  EXPECT_EQ(NormalizeToString(kCmdline, sizeof(kCmdline)), "");
}

TEST(ProcUtilsTest, NormalizeTrailingAt) {
  char kCmdline[] = "foo@";
  EXPECT_EQ(NormalizeToString(kCmdline, sizeof(kCmdline)), "foo");
}

TEST(ProcUtilsTest, NormalizeOnlyTrailingAt) {
  char kCmdline[] = "@";
  EXPECT_EQ(NormalizeToString(kCmdline, sizeof(kCmdline)), "");
}

TEST(ProcUtilsTest, NormalizeTrailingSlash) {
  char kCmdline[] = "foo/";
  EXPECT_EQ(NormalizeToString(kCmdline, sizeof(kCmdline)), "");
}

TEST(ProcUtilsTest, NormalizeOnlySlash) {
  char kCmdline[] = "/";
  EXPECT_EQ(NormalizeToString(kCmdline, sizeof(kCmdline)), "");
}

TEST(ProcUtilsTest, NormalizeTwoArgsSlash) {
  char kCmdline[] = "surfaceflinger/\0--foo";
  EXPECT_EQ(NormalizeToString(kCmdline, sizeof(kCmdline)), "");
}

TEST(ProcUtilsTest, NormalizeEmptyFirstArg) {
  char kCmdline[] = "\0--foo";
  EXPECT_EQ(NormalizeToString(kCmdline, sizeof(kCmdline)), "");
}

TEST(ProcUtilsTest, NormalizeNoNullTerminated) {
  char kCmdline[] = {'f'};
  char* cmdline = kCmdline;
  EXPECT_EQ(NormalizeCmdLine(&cmdline, sizeof(kCmdline)), -1);
}

TEST(ProcUtilsTest, NormalizeZeroLength) {
  char* cmdline = nullptr;
  EXPECT_EQ(NormalizeCmdLine(&cmdline, 0), -1);
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

TEST(ProcUtilsTest, GetRssAnonAndSwap) {
  std::string status = "Name: foo\nRssAnon:  10000 kB\nVmSwap:\t10000 kB";
  EXPECT_EQ(GetRssAnonAndSwap(status), 20000u);
}

TEST(ProcUtilsTest, GetRssAnonAndSwapInvalidInput) {
  EXPECT_EQ(GetRssAnonAndSwap(""), std::nullopt);
  EXPECT_EQ(GetRssAnonAndSwap("RssAnon: 10000 kB"), std::nullopt);
  EXPECT_EQ(GetRssAnonAndSwap("VmSwap: 10000"), std::nullopt);
}

TEST(ProcUtilsTest, GetUids) {
  std::string status =
      "Name: foo\nRssAnon:  10000 kB\nVmSwap:\t10000 kB\n"
      "Uid: 1 2 3 4\n";
  auto uids = GetUids(status);
  ASSERT_NE(uids, std::nullopt);
  EXPECT_EQ(uids->real, 1u);
  EXPECT_EQ(uids->effective, 2u);
  EXPECT_EQ(uids->saved_set, 3u);
  EXPECT_EQ(uids->filesystem, 4u);
}

TEST(ProcUtilsTest, GetUidsInvalidInt) {
  std::string status =
      "Name: foo\nRssAnon:  10000 kB\nVmSwap:\t10000 kB\n"
      "Uid: 1a 2 3 4\n";
  auto uids = GetUids(status);
  EXPECT_EQ(uids, std::nullopt);
}

TEST(ProcUtilsTest, GetUidsInvalidTooFew) {
  std::string status =
      "Name: foo\nRssAnon:  10000 kB\nVmSwap:\t10000 kB\n"
      "Uid: 1 2 3\n";
  auto uids = GetUids(status);
  EXPECT_EQ(uids, std::nullopt);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
