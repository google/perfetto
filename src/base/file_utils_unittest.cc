/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "perfetto/ext/base/file_utils.h"

#include "perfetto/base/build_config.h"
#include "test/gtest_and_gmock.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#include <libgen.h>
#include <cstring>
#endif

namespace perfetto {
namespace base {
namespace {

TEST(FileUtilsTest, Basename) {
  EXPECT_EQ(Basename("/usr/bin/ls"), "ls");
  EXPECT_EQ(Basename("/usr/bin"), "bin");
  EXPECT_EQ(Basename("/usr/"), "usr");
  EXPECT_EQ(Basename("/usr"), "usr");
  EXPECT_EQ(Basename("/"), "/");
  EXPECT_EQ(Basename("///"), "/");
  EXPECT_EQ(Basename("//usr//bin//"), "bin");
  EXPECT_EQ(Basename("foo"), "foo");
  EXPECT_EQ(Basename("foo/bar"), "bar");
  EXPECT_EQ(Basename(""), ".");

  // Windows paths.
  EXPECT_EQ(Basename("C:\\Windows\\System32"), "System32");
  EXPECT_EQ(Basename("C:\\Windows\\"), "Windows");
  EXPECT_EQ(Basename("C:\\Windows"), "Windows");
  EXPECT_EQ(Basename("C:\\"), "C:");
  EXPECT_EQ(Basename("\\"), "\\");
  EXPECT_EQ(Basename("\\\\\\"), "\\");
  EXPECT_EQ(Basename("foo\\bar"), "bar");
  EXPECT_EQ(Basename("foo\\bar\\"), "bar");

  // Mixed cases.
  EXPECT_EQ(Basename("C:/Windows/System32"), "System32");
  EXPECT_EQ(Basename("foo/bar\\baz"), "baz");
  EXPECT_EQ(Basename("foo\\bar/baz"), "baz");
  EXPECT_EQ(Basename("foo/bar\\"), "bar");
}

TEST(FileUtilsTest, Dirname) {
  EXPECT_EQ(Dirname("/usr/bin/ls"), "/usr/bin");
  EXPECT_EQ(Dirname("/usr/bin"), "/usr");
  EXPECT_EQ(Dirname("/usr/"), "/");
  EXPECT_EQ(Dirname("/usr"), "/");
  EXPECT_EQ(Dirname("/"), "/");
  EXPECT_EQ(Dirname("///"), "/");
  EXPECT_EQ(Dirname("//usr//bin//"), "//usr");
  EXPECT_EQ(Dirname("foo"), ".");
  EXPECT_EQ(Dirname("foo/bar"), "foo");
  EXPECT_EQ(Dirname(""), ".");

  // Windows.
  EXPECT_EQ(Dirname("C:\\Windows\\System32"), "C:\\Windows");
  EXPECT_EQ(Dirname("C:\\Windows\\"), "C:");
  EXPECT_EQ(Dirname("C:\\Windows"), "C:");
  EXPECT_EQ(Dirname("\\"), "\\");
  EXPECT_EQ(Dirname("\\\\\\"), "\\");
  EXPECT_EQ(Dirname("foo\\bar"), "foo");
  EXPECT_EQ(Dirname("foo\\bar\\"), "foo");

  // Mixed.
  EXPECT_EQ(Dirname("C:/Windows/System32"), "C:/Windows");
  EXPECT_EQ(Dirname("foo/bar\\baz"), "foo/bar");
  EXPECT_EQ(Dirname("foo\\bar/baz"), "foo\\bar");
  EXPECT_EQ(Dirname("foo/bar\\"), "foo");
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
// Test that our Basename/Dirname implementation matches the behavior of the
// POSIX libgen versions for Unix paths.
TEST(FileUtilsTest, BasenameAndDirnameVsLibgen) {
  // Note: libgen's basename() and dirname() modify their input, so we need
  // to make a copy for each call.
  auto test_basename = [](const char* path) {
    std::string our_result = Basename(path);
    std::string path_copy = path;
    const char* libgen_result = ::basename(path_copy.data());
    EXPECT_EQ(our_result, libgen_result) << "Path: " << path;
  };

  auto test_dirname = [](const char* path) {
    std::string our_result = Dirname(path);
    std::string path_copy = path;
    const char* libgen_result = ::dirname(path_copy.data());
    EXPECT_EQ(our_result, libgen_result) << "Path: " << path;
  };

  // Test various Unix-style paths
  const char* test_paths[] = {
      "/usr/bin/ls",                //
      "/usr/bin",                   //
      "/usr/",                      //
      "/usr",                       //
      "/",                          //
      "///",                        //
      "//usr//bin//",               //
      "foo",                        //
      "foo/bar",                    //
      "foo/bar/",                   //
      "",                           //
      "relative/path/to/file.txt",  //
      "./foo",                      //
      "../bar",                     //
      "a/b/c/d/e/f",                //
  };

  for (const char* path : test_paths) {
    test_basename(path);
    test_dirname(path);
  }
}
#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||
        // PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) ||
        // PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)

}  // namespace
}  // namespace base
}  // namespace perfetto
