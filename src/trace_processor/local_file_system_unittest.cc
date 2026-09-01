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

#include "src/trace_processor/local_file_system.h"

#include <memory>
#include <string>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::io {
namespace {

TEST(LocalFileSystemTest, WritesAndReplacesFile) {
  base::TempDir dir = base::TempDir::Create();
  std::string path = dir.path() + "/output";
  auto file_system = CreateLocalFileSystem();

  FileOpenOptions options;
  options.access = FileAccess::kWriteOnly;
  options.create = true;
  options.truncate = true;
  {
    std::unique_ptr<File> file;
    ASSERT_OK(file_system->OpenFile(path, options, &file));
    ASSERT_OK(file->WriteAt(0, "abc", 3));
  }

  std::string contents;
  ASSERT_TRUE(base::ReadFile(path, &contents));
  EXPECT_EQ(contents, "abc");

  {
    std::unique_ptr<File> file;
    ASSERT_OK(file_system->OpenFile(path, options, &file));
    ASSERT_OK(file->WriteAt(0, "d", 1));
  }

  contents.clear();
  ASSERT_TRUE(base::ReadFile(path, &contents));
  EXPECT_EQ(contents, "d");
  ASSERT_OK(file_system->DeleteFile(path));
}

TEST(LocalFileSystemTest, RandomAccessReadWrite) {
  base::TempDir dir = base::TempDir::Create();
  std::string path = dir.path() + "/output";
  auto file_system = CreateLocalFileSystem();

  FileOpenOptions options;
  options.access = FileAccess::kReadWrite;
  options.create = true;
  options.truncate = true;
  std::unique_ptr<File> file;
  ASSERT_OK(file_system->OpenFile(path, options, &file));
  ASSERT_OK(file->WriteAt(10, "hello", 5));
  ASSERT_OK(file->WriteAt(0, "start ", 6));
  ASSERT_OK(file->Flush());
  uint64_t size = 0;
  ASSERT_OK(file->GetSize(&size));
  EXPECT_EQ(size, 15u);

  char buffer[16] = {};
  size_t read = 0;
  ASSERT_OK(file->ReadAt(10, buffer, 7, &read));
  EXPECT_EQ(read, 5u);
  EXPECT_EQ(std::string(buffer, 5), "hello");
  // The gap between the two writes is a sparse hole read back as zeros.
  ASSERT_OK(file->ReadAt(0, buffer, 10, &read));
  EXPECT_EQ(read, 10u);
  EXPECT_EQ(std::string(buffer, 6), "start ");
  EXPECT_EQ(std::string(buffer + 6, 4), std::string(4, '\0'));

  ASSERT_OK(file->Truncate(6));
  ASSERT_OK(file->GetSize(&size));
  EXPECT_EQ(size, 6u);

  file.reset();
  ASSERT_OK(file_system->DeleteFile(path));
}

TEST(LocalFileSystemTest, RejectsReadOnlyCreate) {
  base::TempDir dir = base::TempDir::Create();
  std::string path = dir.path() + "/output";
  auto file_system = CreateLocalFileSystem();

  FileOpenOptions options;
  options.access = FileAccess::kReadOnly;
  options.create = true;
  std::unique_ptr<File> file;
  EXPECT_THAT(file_system->OpenFile(path, options, &file),
              base::gtest_matchers::IsError());
}

TEST(LocalFileSystemTest, NoopFileSystemRejectsAllOperations) {
  auto file_system = CreateNoopFileSystem();

  FileOpenOptions options;
  options.access = FileAccess::kReadWrite;
  options.create = true;
  std::unique_ptr<File> file;
  EXPECT_THAT(file_system->OpenFile("path", options, &file),
              base::gtest_matchers::IsError());
  EXPECT_THAT(file_system->DeleteFile("path"), base::gtest_matchers::IsError());
  bool exists = false;
  EXPECT_THAT(file_system->FileExists("path", &exists),
              base::gtest_matchers::IsError());
}

TEST(FileSystemConfigTest, DisabledByDefault) {
  EXPECT_FALSE(Config().enable_sql_file_access);
}

}  // namespace
}  // namespace perfetto::trace_processor::io
