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

#include "src/trace_processor/shell/convert_helpers.h"

#include <fstream>
#include <istream>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::shell {
namespace {

std::string PathIn(const base::TempDir& dir, const char* name) {
  return dir.path() + "/" + name;
}

base::TempFile TempFileWith(const std::string& content) {
  base::TempFile file = base::TempFile::Create();
  PERFETTO_CHECK(base::WriteAll(file.fd(), content.data(), content.size()) ==
                 static_cast<ssize_t>(content.size()));
  return file;
}

TEST(ConvertHelpersTest, OpenConversionInputReadsFile) {
  base::TempFile file = TempFileWith("hello world");

  std::ifstream owned;
  std::istream* stream = nullptr;
  base::Status s = OpenConversionInput(file.path(), &owned, &stream);
  ASSERT_TRUE(s.ok()) << s.c_message();
  ASSERT_NE(stream, nullptr);

  std::string content((std::istreambuf_iterator<char>(*stream)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "hello world");
}

TEST(ConvertHelpersTest, OpenConversionInputMissingFileFails) {
  base::TempDir dir = base::TempDir::Create();
  std::ifstream owned;
  std::istream* stream = nullptr;
  base::Status s =
      OpenConversionInput(PathIn(dir, "does_not_exist"), &owned, &stream);
  EXPECT_FALSE(s.ok());
}

TEST(ConvertHelpersTest, OpenConversionOutputWritesFile) {
  // A path rather than a TempFile: on Windows an open TempFile lacks
  // FILE_SHARE_WRITE, so reopening it for write hits a sharing violation.
  base::TempDir dir = base::TempDir::Create();
  std::string path = PathIn(dir, "out.bin");

  std::ofstream owned;
  std::ostream* stream = nullptr;
  base::Status s =
      OpenConversionOutput(path, /*binary_output=*/true, &owned, &stream);
  ASSERT_TRUE(s.ok()) << s.c_message();
  ASSERT_NE(stream, nullptr);

  *stream << "payload";
  owned.flush();
  owned.close();

  std::string content;
  ASSERT_TRUE(base::ReadFile(path, &content));
  EXPECT_EQ(content, "payload");

  base::Unlink(path.c_str());  // TempDir's destructor needs an empty dir.
}

TEST(ConvertHelpersTest, OpenConversionOutputBadPathFails) {
  base::TempDir dir = base::TempDir::Create();
  std::ofstream owned;
  std::ostream* stream = nullptr;
  // The intermediate directory does not exist, so the open must fail.
  base::Status s =
      OpenConversionOutput(PathIn(dir, "no_such_dir/out.bin"),
                           /*binary_output=*/true, &owned, &stream);
  EXPECT_FALSE(s.ok());
}

TEST(ConvertHelpersTest, TextToTraceEmptyInputSucceeds) {
  std::istringstream in("");
  std::ostringstream out;
  EXPECT_EQ(TextToTrace(&in, &out), 0);
  // An empty trace serializes to zero bytes.
  EXPECT_TRUE(out.str().empty());
}

TEST(ConvertHelpersTest, TextToTraceValidProtoSucceeds) {
  std::istringstream in("packet { timestamp: 42 }");
  std::ostringstream out;
  EXPECT_EQ(TextToTrace(&in, &out), 0);
  EXPECT_FALSE(out.str().empty());
}

TEST(ConvertHelpersTest, TextToTraceInvalidProtoFails) {
  std::istringstream in("this is not a valid trace proto }}}");
  std::ostringstream out;
  EXPECT_EQ(TextToTrace(&in, &out), 1);
}

}  // namespace
}  // namespace perfetto::trace_processor::shell
