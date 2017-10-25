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

#include "cpp_common/base.h"

#include <fcntl.h>
#include <unistd.h>

#include "gtest/gtest.h"

namespace perfetto {
namespace {

TEST(ScopedFD, CloseOutOfScope) {
  int raw_fd = open("/dev/null", O_RDONLY);
  ASSERT_GE(raw_fd, 0);
  {
    ScopedFile scoped_file(raw_fd);
    ASSERT_GE(scoped_file.get(), 0);
  }
  ASSERT_NE(0, close(raw_fd));  // close() should fail if the fd is closed.
}

TEST(ScopedFD, Reset) {
  int raw_fd1 = open("/dev/null", O_RDONLY);
  int raw_fd2 = open("/dev/zero", O_RDONLY);
  ASSERT_GE(raw_fd1, 0);
  ASSERT_GE(raw_fd2, 0);
  {
    ScopedFile scoped_file(raw_fd1);
    ASSERT_EQ(raw_fd1, scoped_file.get());
    scoped_file.reset(raw_fd2);
    ASSERT_EQ(raw_fd2, scoped_file.get());
    ASSERT_NE(0, close(raw_fd1));  // close() should fail if the fd is closed.
    scoped_file.reset();
    ASSERT_NE(0, close(raw_fd2));
    scoped_file.reset(open("/dev/null", O_RDONLY));
    ASSERT_GE(scoped_file.get(), 0);
  }
}

TEST(ScopedFD, MoveCtor) {
  int raw_fd1 = open("/dev/null", O_RDONLY);
  int raw_fd2 = open("/dev/zero", O_RDONLY);
  ASSERT_GE(raw_fd1, 0);
  ASSERT_GE(raw_fd2, 0);
  {
    ScopedFile scoped_file1(ScopedFile{raw_fd1});
    ScopedFile scoped_file2(std::move(scoped_file1));
    ASSERT_EQ(-1, scoped_file1.get());
    ASSERT_EQ(raw_fd1, scoped_file2.get());

    scoped_file1.reset(raw_fd2);
    ASSERT_EQ(raw_fd2, scoped_file1.get());
  }
  ASSERT_NE(0, close(raw_fd1));  // close() should fail if the fd is closed.
  ASSERT_NE(0, close(raw_fd2));
}

TEST(ScopedFD, MoveAssignment) {
  int raw_fd1 = open("/dev/null", O_RDONLY);
  int raw_fd2 = open("/dev/zero", O_RDONLY);
  ASSERT_GE(raw_fd1, 0);
  ASSERT_GE(raw_fd2, 0);
  {
    ScopedFile scoped_file1(raw_fd1);
    ScopedFile scoped_file2(raw_fd2);
    scoped_file2 = std::move(scoped_file1);
    ASSERT_EQ(-1, scoped_file1.get());
    ASSERT_EQ(raw_fd1, scoped_file2.get());
    ASSERT_NE(0, close(raw_fd2));

    scoped_file1 = std::move(scoped_file2);
    ASSERT_EQ(raw_fd1, scoped_file1.get());
    ASSERT_EQ(-1, scoped_file2.get());
  }
  ASSERT_NE(0, close(raw_fd1));
}

// File descriptors are capabilities and hence can be security critical. A
// failed close() suggests the memory ownership of the file is wrong and we
// might have leaked a capability.
TEST(ScopedFD, CloseFailureIsFatal) {
  int raw_fd = open("/dev/null", O_RDONLY);
  ASSERT_DEATH(
      {
        ScopedFile scoped_file(raw_fd);
        ASSERT_EQ(0, close(raw_fd));
      },
      "");
}

}  // namespace
}  // namespace perfetto
