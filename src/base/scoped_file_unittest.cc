
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

#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/base/build_config.h"

#include <fcntl.h>

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <io.h>
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
#include <lib/fdio/fdio.h>
#else
#include <unistd.h>
// Double closing of file handles on Windows leads to invocation of the invalid
// parameter handler or asserts and therefore it cannot be tested, but it can
// be tested on other platforms.
#define TEST_INVALID_CLOSE
#endif

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

int OpenDevNull() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
  return fdio_fd_create_null();
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return open("NUL", O_RDONLY);
#else
  return open("/dev/null", O_RDONLY);
#endif
}

FILE* OpenDevNullStream() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
  return fdopen(fdio_fd_create_null(), "r");
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return fopen("NUL", "r");
#else
  return fopen("/dev/null", "r");
#endif
}

// Returns a file descriptor to some file. On Fuchsia: returns a descriptor of a
// file in /tmp. On other platforms: returns a descriptor of /dev/zero.
int MakeSecondFileDescriptor() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
  // Create a random file in /tmp and unlink it straight away since its name
  // never need be known or uttered.
  char path[] = "/tmp/sfuXXXXXX";
  const int fd = mkstemp(&path[0]);
  if (fd >= 0)
    unlink(path);
  return fd;
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return open("NUL", O_RDONLY);
#else
  return open("/dev/zero", O_RDONLY);
#endif
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
TEST(ScopedDirTest, CloseOutOfScope) {
  DIR* dir_handle = opendir(".");
  ASSERT_NE(nullptr, dir_handle);
  int dir_handle_fd = dirfd(dir_handle);
  ASSERT_GE(dir_handle_fd, 0);
  {
    ScopedDir scoped_dir(dir_handle);
    ASSERT_EQ(dir_handle, scoped_dir.get());
    ASSERT_TRUE(scoped_dir);
  }
  ASSERT_NE(0, close(dir_handle_fd));  // Should fail when closing twice.
}
#endif

TEST(ScopedFileTest, CloseOutOfScope) {
  int raw_fd = OpenDevNull();
  ASSERT_GE(raw_fd, 0);
  {
    ScopedFile scoped_file(raw_fd);
    ASSERT_EQ(raw_fd, scoped_file.get());
    ASSERT_EQ(raw_fd, *scoped_file);
    ASSERT_TRUE(scoped_file);
  }
#ifdef TEST_INVALID_CLOSE
  ASSERT_NE(0, close(raw_fd));  // Should fail when closing twice.
#endif
}

TEST(ScopedFstreamTest, CloseOutOfScope) {
  FILE* raw_stream = OpenDevNullStream();
  ASSERT_NE(nullptr, raw_stream);
  {
    ScopedFstream scoped_stream(raw_stream);
    ASSERT_EQ(raw_stream, scoped_stream.get());
    ASSERT_EQ(raw_stream, *scoped_stream);
    ASSERT_TRUE(scoped_stream);
  }
  // We don't have a direct way to see that the file was closed.
}

TEST(ScopedFileTest, Reset) {
  int raw_fd1 = OpenDevNull();
  int raw_fd2 = MakeSecondFileDescriptor();
  ASSERT_GE(raw_fd1, 0);
  ASSERT_GE(raw_fd2, 0);
  {
    ScopedFile scoped_file(raw_fd1);
    ASSERT_EQ(raw_fd1, scoped_file.get());
    scoped_file.reset(raw_fd2);
    ASSERT_EQ(raw_fd2, scoped_file.get());
#ifdef TEST_INVALID_CLOSE
    ASSERT_NE(0, close(raw_fd1));  // Should fail when closing twice.
#endif
    scoped_file.reset();
#ifdef TEST_INVALID_CLOSE
    ASSERT_NE(0, close(raw_fd2));
#endif
    scoped_file.reset(OpenDevNull());
    ASSERT_GE(scoped_file.get(), 0);
  }
}

TEST(ScopedFileTest, Release) {
  int raw_fd = OpenDevNull();
  ASSERT_GE(raw_fd, 0);
  {
    ScopedFile scoped_file(raw_fd);
    ASSERT_EQ(raw_fd, scoped_file.release());
    ASSERT_FALSE(scoped_file);
  }
  ASSERT_EQ(0, close(raw_fd));
}

TEST(ScopedFileTest, MoveCtor) {
  int raw_fd1 = OpenDevNull();
  int raw_fd2 = MakeSecondFileDescriptor();
  ASSERT_GE(raw_fd1, 0);
  ASSERT_GE(raw_fd2, 0);
  {
    ScopedFile scoped_file1(ScopedFile{raw_fd1});
    ScopedFile scoped_file2(std::move(scoped_file1));
    ASSERT_EQ(-1, scoped_file1.get());
    ASSERT_EQ(-1, *scoped_file1);
    ASSERT_FALSE(scoped_file1);
    ASSERT_EQ(raw_fd1, scoped_file2.get());

    scoped_file1.reset(raw_fd2);
    ASSERT_EQ(raw_fd2, scoped_file1.get());
  }
#ifdef TEST_INVALID_CLOSE
  ASSERT_NE(0, close(raw_fd1));  // Should fail when closing twice.
  ASSERT_NE(0, close(raw_fd2));
#endif
}

TEST(ScopedFileTest, MoveAssignment) {
  int raw_fd1 = OpenDevNull();
  int raw_fd2 = MakeSecondFileDescriptor();
  ASSERT_GE(raw_fd1, 0);
  ASSERT_GE(raw_fd2, 0);
  {
    ScopedFile scoped_file1(raw_fd1);
    ScopedFile scoped_file2(raw_fd2);
    scoped_file2 = std::move(scoped_file1);
    ASSERT_EQ(-1, scoped_file1.get());
    ASSERT_FALSE(scoped_file1);
    ASSERT_EQ(raw_fd1, scoped_file2.get());
#ifdef TEST_INVALID_CLOSE
    ASSERT_NE(0, close(raw_fd2));
#endif

    scoped_file1 = std::move(scoped_file2);
    ASSERT_EQ(raw_fd1, scoped_file1.get());
    ASSERT_EQ(-1, scoped_file2.get());
  }
#ifdef TEST_INVALID_CLOSE
  ASSERT_NE(0, close(raw_fd1));
#endif
}

// File descriptors are capabilities and hence can be security critical. A
// failed close() suggests the memory ownership of the file is wrong and we
// might have leaked a capability.
#ifdef TEST_INVALID_CLOSE
TEST(ScopedFileTest, CloseFailureIsFatal) {
  int raw_fd = OpenDevNull();
  ASSERT_DEATH_IF_SUPPORTED(
      {
        ScopedFile scoped_file(raw_fd);
        ASSERT_EQ(0, close(raw_fd));
      },
      "");
}
#endif

}  // namespace
}  // namespace base
}  // namespace perfetto
