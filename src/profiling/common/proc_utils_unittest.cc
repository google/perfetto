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
#include "src/base/test/tmp_dir_tree.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
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

TEST(ProcUtilsTest, NormalizeCmdlinesExactOnly) {
  auto patterns = NormalizeCmdlines(
      {"/system/bin/surfaceflinger@1.0", "com.android.phone"});
  ASSERT_TRUE(patterns.has_value());
  EXPECT_THAT(patterns->exact_patterns,
              ElementsAre("surfaceflinger", "com.android.phone"));
  EXPECT_THAT(patterns->glob_patterns, IsEmpty());
}

TEST(ProcUtilsTest, NormalizeCmdlinesSplitsExactAndGlob) {
  auto patterns = NormalizeCmdlines(
      {"/system/bin/surfaceflinger@1.0", "com.android.*", "system_server"});
  ASSERT_TRUE(patterns.has_value());
  EXPECT_THAT(patterns->exact_patterns,
              ElementsAre("surfaceflinger", "system_server"));
  // Glob patterns are kept verbatim (not normalized).
  EXPECT_THAT(patterns->glob_patterns, ElementsAre("com.android.*"));
}

TEST(ProcUtilsTest, NormalizeCmdlinesGlobKeptVerbatim) {
  auto patterns = NormalizeCmdlines({"/system/bin/*", "*@2.0"});
  ASSERT_TRUE(patterns.has_value());
  EXPECT_THAT(patterns->exact_patterns, IsEmpty());
  EXPECT_THAT(patterns->glob_patterns, ElementsAre("/system/bin/*", "*@2.0"));
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

TEST(ProcUtilsTest, PidMatchesCmdlinePatterns) {
  EXPECT_TRUE(glob_aware::PidMatchesCmdlinePatterns(getpid(), {"*"}));
  EXPECT_FALSE(glob_aware::PidMatchesCmdlinePatterns(
      getpid(), {"no_such_binary_for_sure"}));
  EXPECT_FALSE(glob_aware::PidMatchesCmdlinePatterns(getpid(), {}));
}

TEST(ProcUtilsTest, GetUidFromProcfs) {
  EXPECT_EQ(GetUidFromProcfs(getpid()), geteuid());
  EXPECT_EQ(GetUidFromProcfs(-1), std::nullopt);
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

TEST(ProcUtilsTest, GetCgroupV2Path) {
  // Android: cgroup v1 controllers alongside the v2 hierarchy.
  EXPECT_EQ(GetCgroupV2Path("3:cpuset:/top-app\n"
                            "2:cpu:/top-app\n"
                            "1:blkio:/\n"
                            "0::/apps/uid_10123/pid_4567\n"),
            "/apps/uid_10123/pid_4567");
  // Without the apps/system split, and without a trailing newline.
  EXPECT_EQ(GetCgroupV2Path("0::/uid_1000/pid_123"), "/uid_1000/pid_123");
  // Root cgroup.
  EXPECT_EQ(GetCgroupV2Path("0::/\n"), "/");
  // Colons are valid within cgroup names.
  EXPECT_EQ(GetCgroupV2Path("0::/foo:bar/baz\n"), "/foo:bar/baz");
}

TEST(ProcUtilsTest, GetCgroupV2PathInvalidInput) {
  EXPECT_EQ(GetCgroupV2Path(""), std::nullopt);
  // Only cgroup v1 hierarchies.
  EXPECT_EQ(GetCgroupV2Path("2:cpu:/foreground\n1:cpuset:/foreground\n"),
            std::nullopt);
  EXPECT_EQ(GetCgroupV2Path("0::\n"), std::nullopt);
  EXPECT_EQ(GetCgroupV2Path("0::not_absolute\n"), std::nullopt);
  // The cgroup has been removed (e.g. the process is a zombie).
  EXPECT_EQ(GetCgroupV2Path("0::/apps/uid_10123/pid_4567 (deleted)\n"),
            std::nullopt);
}

TEST(ProcUtilsTest, IsCgroupV2Frozen) {
  base::TmpDirTree cgroups;
  cgroups.AddDir("frozen");
  cgroups.AddFile("frozen/cgroup.freeze", "1\n");
  cgroups.AddDir("thawed");
  cgroups.AddFile("thawed/cgroup.freeze", "0\n");
  cgroups.AddDir("empty");
  cgroups.AddFile("empty/cgroup.freeze", "");
  cgroups.AddDir("no_freezer");

  EXPECT_TRUE(IsCgroupV2Frozen(cgroups.AbsolutePath("frozen")));
  EXPECT_FALSE(IsCgroupV2Frozen(cgroups.AbsolutePath("thawed")));
  EXPECT_FALSE(IsCgroupV2Frozen(cgroups.AbsolutePath("empty")));
  // No cgroup.freeze file (e.g. the root cgroup, or no freezer support).
  EXPECT_FALSE(IsCgroupV2Frozen(cgroups.AbsolutePath("no_freezer")));
  EXPECT_FALSE(IsCgroupV2Frozen(cgroups.AbsolutePath("does_not_exist")));
}

TEST(ProcUtilsTest, IsProcessFrozen) {
  // The test itself is evidently not frozen.
  EXPECT_FALSE(IsProcessFrozen(getpid()));
  // Not a valid process.
  EXPECT_FALSE(IsProcessFrozen(-1));
}

TEST(ProcUtilsTest, RemoveFrozenProcesses) {
  std::set<pid_t> pids = {getpid()};
  RemoveFrozenProcesses(&pids);
  EXPECT_THAT(pids, ElementsAre(getpid()));
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
