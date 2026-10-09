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

#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/file_utils.h"

#include <stdlib.h>
#include <sys/stat.h>

#include <optional>
#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/utils.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <unistd.h>
#endif

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
constexpr char kTempDirEnvVar[] = "TMP";
#else
constexpr char kTempDirEnvVar[] = "TMPDIR";
#endif

bool PathExists(const std::string& path) {
  struct stat stat_buf;
  return stat(path.c_str(), &stat_buf) == 0;
}

TEST(TempFileTest, MaybeCreate) {
  std::string path;
  int fd;
  {
    TempFile tf = TempFile::MaybeCreate();
    path = tf.path();
    fd = tf.fd();
    ASSERT_NE("", path);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(PathExists(path));
    ASSERT_GE(write(fd, "foo", 4), 0);

    TempFile moved_tf(std::move(tf));
    ASSERT_EQ("", tf.path());
    ASSERT_EQ(-1, tf.fd());
    ASSERT_EQ(path, moved_tf.path());
    ASSERT_EQ(fd, moved_tf.fd());
    ASSERT_GE(write(moved_tf.fd(), "foo", 4), 0);

    TempFile moved_tf2 = std::move(moved_tf);
    ASSERT_EQ("", moved_tf.path());
    ASSERT_EQ(-1, moved_tf.fd());
    ASSERT_EQ(path, moved_tf2.path());
    ASSERT_EQ(fd, moved_tf2.fd());
    ASSERT_GE(write(moved_tf2.fd(), "foo", 4), 0);
  }

  // The file should be deleted and closed now.
  ASSERT_FALSE(PathExists(path));

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // Windows UCRT aborts when trying to write into a closed FD.
  ASSERT_EQ(-1, write(fd, "foo", 4));
#endif
}

TEST(TempFileTest, ReopenForWriting) {
  TempFile file = TempFile::CreateFileForTest();
  ASSERT_EQ(WriteAll(file.fd(), "old", 3), 3);

  ScopedFile reopened = OpenFile(file.path(), O_RDWR | O_TRUNC);
  ASSERT_TRUE(reopened);
  ASSERT_EQ(WriteAll(*reopened, "new", 3), 3);
  reopened.reset();

  std::string contents;
  ASSERT_TRUE(ReadFile(file.path(), &contents));
  EXPECT_EQ(contents, "new");
}

TEST(TempFileTest, MaybeCreateUnlinked) {
  int fd;
  {
    TempFile tf = TempFile::MaybeCreateUnlinked();
    ASSERT_EQ("", tf.path());
    fd = tf.fd();
    ASSERT_GE(fd, 0);
    ASSERT_GE(write(fd, "foo", 4), 0);
  }

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // Windows UCRT aborts when trying to write into a closed FD.
  ASSERT_EQ(-1, write(fd, "foo", 4));
#endif
}

TEST(TempFileTest, MaybeCreateUnlinkedReleaseFD) {
  TempFile file = TempFile::MaybeCreateUnlinked();
  ASSERT_GE(file.fd(), 0);
  EXPECT_TRUE(file.path().empty());
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  struct stat stat_buf = {};
  ASSERT_EQ(fstat(file.fd(), &stat_buf), 0);
  EXPECT_EQ(stat_buf.st_nlink, 0u);
#endif
  ScopedFile fd = file.ReleaseFD();
  EXPECT_EQ(file.fd(), -1);
  ASSERT_EQ(WriteAll(*fd, "foo", 3), 3);
  ASSERT_TRUE(SeekFile(*fd, 0));
  std::string contents;
  ASSERT_TRUE(ReadFileDescriptor(*fd, &contents));
  EXPECT_EQ(contents, "foo");
}

TEST(TempFileTest, CreationFailureReturnsInvalidFile) {
  // A regular file cannot serve as a temporary directory.
  TempFile parent = TempFile::CreateFileForTest();
  const char* value = getenv(kTempDirEnvVar);
  const auto saved = value ? std::optional<std::string>(value) : std::nullopt;
  SetEnv(kTempDirEnvVar, parent.path());
  TempFile file = TempFile::MaybeCreate();
  TempFile unlinked_file = TempFile::MaybeCreateUnlinked();
  if (saved) {
    SetEnv(kTempDirEnvVar, *saved);
  } else {
    UnsetEnv(kTempDirEnvVar);
  }
  EXPECT_EQ(file.fd(), -1);
  EXPECT_TRUE(file.path().empty());
  EXPECT_FALSE(file.ReleaseFD());
  EXPECT_EQ(unlinked_file.fd(), -1);
  EXPECT_TRUE(unlinked_file.path().empty());
  EXPECT_FALSE(unlinked_file.ReleaseFD());
}

TEST(TempFileTest, UnlinkClearsPath) {
  TempFile file = TempFile::CreateFileForTest();
  const std::string path = file.path();
  EXPECT_TRUE(file.Unlink());
  EXPECT_TRUE(file.path().empty());
  EXPECT_FALSE(PathExists(path));
  // Unlink() also succeeds if the file is already unlinked.
  EXPECT_TRUE(file.Unlink());
  // The descriptor stays open.
  EXPECT_EQ(WriteAll(file.fd(), "foo", 3), 3);
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
TEST(TempFileTest, UnlinkFailureKeepsPath) {
  TempFile file = TempFile::CreateFileForTest();
  const std::string path = file.path();
  ASSERT_EQ(unlink(path.c_str()), 0);
  EXPECT_FALSE(file.Unlink());
  EXPECT_EQ(file.path(), path);

  // The destructor aborts if it cannot unlink the path, so create it again.
  ASSERT_TRUE(OpenFile(path, O_CREAT | O_RDWR, 0600));
  EXPECT_TRUE(file.Unlink());
  EXPECT_FALSE(PathExists(path));
}
#endif

TEST(TempFileTest, ReleaseUnlinked) {
  ScopedFile fd;
  {
    TempFile tf = TempFile::CreateFileForTest();
    fd = tf.ReleaseFD();
  }
  ASSERT_GE(write(*fd, "foo", 4), 0);
}

TEST(TempFileTest, ReleaseLinked) {
  ScopedFile fd;
  std::string path;
  {
    TempFile tf = TempFile::CreateUnlinkedFileForTest();
    path = tf.path();
    fd = tf.ReleaseFD();
  }

  // The file should be unlinked from the filesystem.
  ASSERT_FALSE(PathExists(path));

  // But still open and writable.
  ASSERT_GE(write(*fd, "foo", 4), 0);
}

TEST(TempFileTest, TempDir) {
  std::string path;
  {
    TempDir td = TempDir::Create();
    ASSERT_NE("", td.path());
    ASSERT_TRUE(PathExists(td.path()));
    path = td.path();
  }
  ASSERT_FALSE(PathExists(path));
}

}  // namespace
}  // namespace base
}  // namespace perfetto
