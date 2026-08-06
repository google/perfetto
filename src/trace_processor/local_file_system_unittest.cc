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
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::io {
namespace {

TEST(LocalFileSystemTest, WritesAndReplacesFile) {
  base::TempDir dir = base::TempDir::Create();
  std::string path = dir.path() + "/output";
  auto file_system = CreateLocalFileSystem();

  {
    std::unique_ptr<File> file;
    base::Status status = file_system->OpenFile(path, &file);
    ASSERT_TRUE(status.ok()) << status.c_message();
    ASSERT_TRUE(file->Write("abc", 3).ok());
  }

  std::string contents;
  ASSERT_TRUE(base::ReadFile(path, &contents));
  EXPECT_EQ(contents, "abc");

  {
    std::unique_ptr<File> file;
    base::Status status = file_system->OpenFile(path, &file);
    ASSERT_TRUE(status.ok()) << status.c_message();
    ASSERT_TRUE(file->Write("d", 1).ok());
  }

  contents.clear();
  ASSERT_TRUE(base::ReadFile(path, &contents));
  EXPECT_EQ(contents, "d");
  ASSERT_TRUE(base::Unlink(path.c_str()));
}

TEST(FileSystemConfigTest, DisabledByDefault) {
  EXPECT_FALSE(Config().enable_sql_file_access);
}

}  // namespace
}  // namespace perfetto::trace_processor::io
