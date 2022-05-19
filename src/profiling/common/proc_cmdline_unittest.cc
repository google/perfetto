/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/profiling/common/proc_cmdline.h"

#include <string>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace glob_aware {
namespace {

TEST(ProcCmdlineTest, FindBinaryNameBinNameOnly) {
  char cmdline[] = "surfaceflinger";
  EXPECT_EQ(cmdline, FindBinaryName(cmdline, sizeof(cmdline) - 1));
}

TEST(ProcCmdlineTest, FindBinaryNameWithArg) {
  char cmdline[] = "surfaceflinger\0--flag";
  EXPECT_EQ(cmdline, FindBinaryName(cmdline, sizeof(cmdline) - 1));
}

TEST(ProcCmdlineTest, FindBinaryNameFullPathAndArgs) {
  char cmdline[] = "/system/bin/surfaceflinger\0--flag\0--flag2";
  EXPECT_STREQ("surfaceflinger", FindBinaryName(cmdline, sizeof(cmdline) - 1));
}

TEST(ProcCmdlineTest, FindBinaryNameSpecialCharsInName) {
  {
    char cmdline[] = "android.hardware.graphics.composer@2.2-service";
    EXPECT_EQ(cmdline, FindBinaryName(cmdline, sizeof(cmdline) - 1));
  }
  {
    char cmdline[] = "com.google.android.googlequicksearchbox:search";
    EXPECT_EQ(cmdline, FindBinaryName(cmdline, sizeof(cmdline) - 1));
  }
  {
    // chrome rewrites cmdline with spaces instead of nul bytes, parsing will
    // therefore treat everything as argv0.
    char cmdline[] =
        "/opt/google/chrome/chrome --type=renderer --enable-crashpad";
    EXPECT_STREQ("chrome --type=renderer --enable-crashpad",
                 FindBinaryName(cmdline, sizeof(cmdline) - 1));
  }
}

TEST(ProcCmdlineTest, FindBinaryNameEdgeCases) {
  {
    char cmdline[] = "";
    EXPECT_STREQ("", FindBinaryName(cmdline, sizeof(cmdline) - 1));
  }
  {
    char cmdline[] = "\0foo";
    EXPECT_STREQ("", FindBinaryName(cmdline, sizeof(cmdline) - 1));
  }
  {
    char cmdline[] = "/foo/";
    EXPECT_STREQ("", FindBinaryName(cmdline, sizeof(cmdline) - 1));
  }
  {
    char cmdline[] = "/";
    EXPECT_STREQ("", FindBinaryName(cmdline, sizeof(cmdline) - 1));
  }
  {
    char cmdline[] = "foo/\0";
    EXPECT_STREQ("", FindBinaryName(cmdline, sizeof(cmdline) - 1));
  }
}

TEST(ProcCmdlineTest, FindAndMatchAbsolutePath) {
  char cmdline[] = "/system/bin/surfaceflinger\0--flag\0--flag2";
  const char* binname = FindBinaryName(cmdline, sizeof(cmdline) - 1);
  ASSERT_TRUE(binname != nullptr);

  EXPECT_TRUE(MatchGlobPattern("/system/bin/surfaceflinger", cmdline, binname));
  EXPECT_TRUE(MatchGlobPattern("/*/surfaceflinger", cmdline, binname));
  EXPECT_TRUE(MatchGlobPattern("surfaceflinger", cmdline, binname));
  EXPECT_TRUE(MatchGlobPattern("???faceflinger", cmdline, binname));
  EXPECT_TRUE(MatchGlobPattern("*", cmdline, binname));

  EXPECT_FALSE(MatchGlobPattern("/system", cmdline, binname));
  EXPECT_FALSE(MatchGlobPattern("bin/surfaceflinger", cmdline, binname));
  EXPECT_FALSE(
      MatchGlobPattern("?system/bin/surfaceflinger", cmdline, binname));
  EXPECT_FALSE(MatchGlobPattern("*/surfaceflinger", cmdline, binname));
}

TEST(ProcCmdlineTest, FindAndMatchRelativePath) {
  char cmdline[] = "./top";
  const char* binname = FindBinaryName(cmdline, sizeof(cmdline) - 1);
  ASSERT_TRUE(binname != nullptr);

  EXPECT_TRUE(MatchGlobPattern("top", cmdline, binname));
  EXPECT_TRUE(MatchGlobPattern("*", cmdline, binname));

  EXPECT_FALSE(MatchGlobPattern("./top", cmdline, binname));
}

}  // namespace
}  // namespace glob_aware
}  // namespace profiling
}  // namespace perfetto
