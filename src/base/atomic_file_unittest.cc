/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "perfetto/ext/base/atomic_file.h"
#include <utility>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::base {
namespace {
std::string Contents(const std::string& path) {
  std::string contents;
  EXPECT_TRUE(ReadFile(path, &contents));
  return contents;
}

TEST(AtomicFileTest, PreservesDestinationUntilCommit) {
  auto destination = TempFile::Create();
  ASSERT_EQ(WriteAll(destination.fd(), "old", 3), 3);
  std::string temporary;
  {
    AtomicFile output(destination.path());
    ASSERT_TRUE(output.Open().ok());
    temporary = output.temp_path();
    auto fd = output.DuplicateFD();
    ASSERT_TRUE(fd);
    ASSERT_EQ(WriteAll(*fd, "new", 3), 3);
    fd.reset();
    EXPECT_EQ(Contents(destination.path()), "old");
    ASSERT_TRUE(std::move(output).Commit().ok());
    EXPECT_EQ(Contents(destination.path()), "new");
  }
  EXPECT_FALSE(FileExists(temporary));
  EXPECT_EQ(Contents(destination.path()), "new");
}

TEST(AtomicFileTest, AbandonPreservesDestinationAndDeletesTemporaryFile) {
  auto destination = TempFile::Create();
  ASSERT_EQ(WriteAll(destination.fd(), "old", 3), 3);
  std::string temporary;
  {
    AtomicFile output(destination.path());
    ASSERT_TRUE(output.Open().ok());
    temporary = output.temp_path();
    auto fd = output.DuplicateFD();
    ASSERT_EQ(WriteAll(*fd, "incomplete", 10), 10);
  }
  EXPECT_FALSE(FileExists(temporary));
  EXPECT_EQ(Contents(destination.path()), "old");
}

TEST(AtomicFileTest, FailedCommitDeletesTemporaryFile) {
  auto directory = TempDir::Create();
  std::string path = directory.path() + "/output";
  std::string temporary;
  {
    AtomicFile output(path);
    ASSERT_TRUE(output.Open().ok());
    temporary = output.temp_path();
    ASSERT_TRUE(Mkdir(path));
    EXPECT_FALSE(std::move(output).Commit().ok());
  }
  EXPECT_TRUE(DirectoryExists(path));
  EXPECT_FALSE(FileExists(temporary));
  ASSERT_TRUE(Rmdir(path));
}

TEST(AtomicFileTest, FailedOpenDoesNotDeleteUnownedFile) {
  auto directory = TempDir::Create();
  std::string temporary;
  {
    AtomicFile output(directory.path() + "/output");
    temporary = output.temp_path();
    auto other = OpenFile(temporary, O_CREAT | O_EXCL | O_WRONLY, 0600);
    ASSERT_TRUE(other);
    ASSERT_EQ(WriteAll(*other, "other", 5), 5);
    EXPECT_FALSE(output.Open().ok());
  }
  EXPECT_EQ(Contents(temporary), "other");
  EXPECT_TRUE(Unlink(temporary.c_str()));
}

TEST(AtomicFileTest, InvalidParentFailsWithoutCreatingDestination) {
  auto directory = TempDir::Create();
  std::string path = directory.path() + "/missing/output";
  AtomicFile output(path);
  EXPECT_FALSE(output.Open().ok());
  EXPECT_FALSE(FileExists(path));
}
}  // namespace
}  // namespace perfetto::base
