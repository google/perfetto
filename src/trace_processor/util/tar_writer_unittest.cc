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

#include "src/trace_processor/util/tar_writer.h"
#include <string.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::util {
namespace {

using perfetto::base::gtest_matchers::IsError;
using testing::ElementsAre;
using testing::HasSubstr;

class TarWriterTest : public ::testing::Test {
 protected:
  TarWriterTest() : temp_file_(base::TempFile::Create()) {}

  void SetUp() override { output_path_ = temp_file_.path(); }

  // Helper to read entire file into string
  std::string ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
  }

  // Helper to create a test file with specific content
  std::string CreateTestFile(const std::string& content) {
    base::TempFile temp_test_file = base::TempFile::Create();
    std::string path = temp_test_file.path();
    std::ofstream file(path);
    file << content;
    file.close();
    created_test_files_.push_back(std::move(temp_test_file));
    return path;
  }

  // Helper to parse TAR header
  struct ParsedTarHeader {
    std::string name;
    size_t size;
    bool is_valid;
  };

  ParsedTarHeader ParseTarHeader(const char* header_data) {
    ParsedTarHeader result = {};

    // Extract filename (null-terminated)
    result.name = std::string(header_data, strnlen(header_data, 100));

    // Extract size (octal string)
    char size_str[13];
    memcpy(size_str, header_data + 124, 12);
    size_str[12] = '\0';

    char* end;
    result.size = static_cast<size_t>(strtol(size_str, &end, 8));
    result.is_valid = (end != size_str);

    return result;
  }

  // Helper to validate TAR structure
  std::vector<ParsedTarHeader> ParseTarFile(const std::string& tar_content) {
    std::vector<ParsedTarHeader> headers;
    size_t offset = 0;

    while (offset + 512 <= tar_content.size()) {
      const char* header_data = tar_content.data() + offset;

      // Check if this is the end marker (all zeros)
      bool is_zero_block = true;
      for (size_t i = 0; i < 512; i++) {
        if (header_data[i] != 0) {
          is_zero_block = false;
          break;
        }
      }

      if (is_zero_block) {
        break;  // Found end of archive
      }

      ParsedTarHeader header = ParseTarHeader(header_data);
      if (!header.is_valid || header.name.empty()) {
        break;  // Invalid header
      }

      headers.push_back(header);

      // Move to next entry (header + content + padding)
      offset += 512;                                // Header
      offset += ((header.size + 511) / 512) * 512;  // Content with padding
    }

    return headers;
  }

  base::TempFile temp_file_;
  std::string output_path_;
  std::vector<base::TempFile> created_test_files_;
};

// TODO(lalitm|sashwinbalaji): Fix test on windows
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#define DisableWindows(x) DISABLED_##x
#else
#define DisableWindows(x) x
#endif

TEST_F(TarWriterTest, DisableWindows(CreateEmptyTar)) {
  {
    TarWriter writer(output_path_);
  }

  ASSERT_TRUE(base::FileExists(output_path_));

  std::string content = ReadFile(output_path_);

  // Empty TAR should have two 512-byte zero blocks
  EXPECT_EQ(content.size(), 1024u);
  for (char c : content) {
    EXPECT_EQ(c, '\0');
  }
}

TEST_F(TarWriterTest, DisableWindows(AddSingleFile)) {
  const std::string test_content = "Hello, TAR world!";

  {
    TarWriter writer(output_path_);
    ASSERT_OK(writer.AddFile("hello.txt", test_content));
  }

  std::string tar_content = ReadFile(output_path_);
  auto headers = ParseTarFile(tar_content);

  ASSERT_EQ(headers.size(), 1u);
  EXPECT_EQ(headers[0].name, "hello.txt");
  EXPECT_EQ(headers[0].size, test_content.size());

  // Verify file content is at the right location
  size_t content_offset = 512;  // After header
  std::string extracted_content =
      tar_content.substr(content_offset, test_content.size());
  EXPECT_EQ(extracted_content, test_content);
}

TEST_F(TarWriterTest, DisableWindows(AddMultipleFiles)) {
  const std::string content1 = "First file content";
  const std::string content2 = "Second file with different content";

  {
    TarWriter writer(output_path_);
    ASSERT_OK(writer.AddFile("file1.txt", content1));
    ASSERT_OK(writer.AddFile("dir/file2.txt", content2));
  }

  std::string tar_content = ReadFile(output_path_);
  auto headers = ParseTarFile(tar_content);

  ASSERT_EQ(headers.size(), 2u);
  EXPECT_EQ(headers[0].name, "file1.txt");
  EXPECT_EQ(headers[0].size, content1.size());
  EXPECT_EQ(headers[1].name, "dir/file2.txt");
  EXPECT_EQ(headers[1].size, content2.size());
}

TEST_F(TarWriterTest, DisableWindows(AddFileFromPath)) {
  const std::string test_content = "File from filesystem";
  std::string test_file_path = CreateTestFile(test_content);

  {
    TarWriter writer(output_path_);
    ASSERT_OK(writer.AddFileFromPath("archived.txt", test_file_path));
  }

  std::string tar_content = ReadFile(output_path_);
  auto headers = ParseTarFile(tar_content);

  ASSERT_EQ(headers.size(), 1u);
  EXPECT_EQ(headers[0].name, "archived.txt");
  EXPECT_EQ(headers[0].size, test_content.size());

  // Verify content
  size_t content_offset = 512;
  std::string extracted_content =
      tar_content.substr(content_offset, test_content.size());
  EXPECT_EQ(extracted_content, test_content);
}

TEST_F(TarWriterTest, DisableWindows(AddFileFromNonexistentPath)) {
  std::string nonexistent_path = "/nonexistent/path/file.txt";

  TarWriter writer(output_path_);
  auto status = writer.AddFileFromPath("archived.txt", nonexistent_path);
  EXPECT_THAT(status, IsError());
}

TEST_F(TarWriterTest, DisableWindows(AddLargeFile)) {
  // Create a large file (larger than typical buffer sizes)
  std::string large_content(100000, 'X');  // 100KB of X's

  {
    TarWriter writer(output_path_);
    ASSERT_OK(writer.AddFile("large.txt", large_content));
  }

  std::string tar_content = ReadFile(output_path_);
  auto headers = ParseTarFile(tar_content);

  ASSERT_EQ(headers.size(), 1u);
  EXPECT_EQ(headers[0].name, "large.txt");
  EXPECT_EQ(headers[0].size, large_content.size());

  // Verify content
  size_t content_offset = 512;
  std::string extracted_content =
      tar_content.substr(content_offset, large_content.size());
  EXPECT_EQ(extracted_content, large_content);
}

TEST_F(TarWriterTest, DisableWindows(ValidateFilenameConstraints)) {
  TarWriter writer(temp_file_.ReleaseFD());

  // Empty filename should fail
  EXPECT_THAT(writer.AddFile("", "content"), IsError());

  // Very long filename should fail (TAR limit is 99 chars for basic format)
  std::string long_name(100, 'a');
  EXPECT_THAT(writer.AddFile(long_name, "content"), IsError());

  // Valid filename at boundary should work
  std::string boundary_name(99, 'b');
  EXPECT_OK(writer.AddFile(boundary_name, "content"));
}

TEST_F(TarWriterTest, DisableWindows(HandleBinaryContent)) {
  // Test with binary data containing null bytes
  std::string binary_content = "Binarydata";
  binary_content[6] = '\0';  // null byte
  binary_content.insert(7, 1, static_cast<char>(0x01));
  binary_content.insert(8, 1, static_cast<char>(0xFF));
  binary_content.insert(9, 1, static_cast<char>(0x7F));
  binary_content.insert(10, 1, static_cast<char>(0x80));

  {
    TarWriter writer(output_path_);
    ASSERT_OK(writer.AddFile("binary.dat", binary_content));
  }

  std::string tar_content = ReadFile(output_path_);
  auto headers = ParseTarFile(tar_content);

  ASSERT_EQ(headers.size(), 1u);
  EXPECT_EQ(headers[0].size, binary_content.size());

  // Verify binary content is preserved
  size_t content_offset = 512;
  std::string extracted_content =
      tar_content.substr(content_offset, binary_content.size());
  EXPECT_EQ(extracted_content, binary_content);
}

TEST_F(TarWriterTest, DisableWindows(PaddingAlignment)) {
  // Test that files are properly padded to 512-byte boundaries
  const std::string content = "X";  // 1 byte content

  {
    TarWriter writer(output_path_);
    ASSERT_OK(writer.AddFile("small.txt", content));
  }

  std::string tar_content = ReadFile(output_path_);

  // Should have: header(512) + content(1) + padding(511) + end_markers(1024)
  EXPECT_EQ(tar_content.size(), 2048u);

  // Verify padding bytes are zero
  for (size_t i = 513; i < 1024; i++) {
    EXPECT_EQ(tar_content[i], '\0')
        << "Padding byte at position " << i << " is not zero";
  }
}

TEST_F(TarWriterTest, DisableWindows(AutomaticFinalization)) {
  // Test that destructor calls Finalize if not already called
  {
    TarWriter writer(output_path_);
    ASSERT_OK(writer.AddFile("test.txt", "content"));
    // Don't explicitly call Finalize()
  }  // Destructor should finalize

  ASSERT_TRUE(base::FileExists(output_path_));

  std::string tar_content = ReadFile(output_path_);
  auto headers = ParseTarFile(tar_content);

  EXPECT_EQ(headers.size(), 1u);
  EXPECT_EQ(headers[0].name, "test.txt");
}

TEST_F(TarWriterTest, DisableWindows(ExplicitFinalizeThenDestructorNoop)) {
  {
    TarWriter writer(output_path_);
    ASSERT_OK(writer.AddFile("test.txt", "content"));
    ASSERT_OK(writer.Finalize());
    // A second explicit call is a no-op that still succeeds.
    ASSERT_OK(writer.Finalize());

    std::string tar_content_after_explicit = ReadFile(output_path_);
    EXPECT_EQ(tar_content_after_explicit.size(), 2048u);
  }  // Destructor calls Finalize() again; must be a no-op.

  std::string tar_content = ReadFile(output_path_);
  EXPECT_EQ(tar_content.size(), 2048u);
}

TEST_F(TarWriterTest, BufferSinkSingleFile) {
  const std::string test_content = "In-memory TAR content";

  std::vector<uint8_t> buffer;
  {
    TarWriter writer(std::make_unique<BufferTarWriterSink>(&buffer));
    ASSERT_OK(writer.AddFile("inmem.txt", test_content));
  }

  std::string tar_content(buffer.begin(), buffer.end());
  auto headers = ParseTarFile(tar_content);

  ASSERT_EQ(headers.size(), 1u);
  EXPECT_EQ(headers[0].name, "inmem.txt");
  EXPECT_EQ(headers[0].size, test_content.size());

  size_t content_offset = 512;
  std::string extracted =
      tar_content.substr(content_offset, test_content.size());
  EXPECT_EQ(extracted, test_content);

  // Two 512-byte zero blocks for end of archive.
  EXPECT_EQ(buffer.size(), 512u + 512u + 1024u);
}

TEST_F(TarWriterTest, BufferSinkMultipleFiles) {
  std::vector<uint8_t> buffer;
  {
    TarWriter writer(std::make_unique<BufferTarWriterSink>(&buffer));
    ASSERT_OK(writer.AddFile("a.txt", "alpha"));
    ASSERT_OK(writer.AddFile("dir/b.txt", "bravo-content"));
  }

  std::string tar_content(buffer.begin(), buffer.end());
  auto headers = ParseTarFile(tar_content);

  ASSERT_EQ(headers.size(), 2u);
  EXPECT_EQ(headers[0].name, "a.txt");
  EXPECT_EQ(headers[0].size, 5u);
  EXPECT_EQ(headers[1].name, "dir/b.txt");
  EXPECT_EQ(headers[1].size, 13u);
}

TEST_F(TarWriterTest, DisableWindows(AddFileRawBytes)) {
  const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x42};

  {
    TarWriter writer(output_path_);
    ASSERT_OK(writer.AddFile("raw.bin", data, sizeof(data)));
  }

  std::string tar_content = ReadFile(output_path_);
  auto headers = ParseTarFile(tar_content);

  ASSERT_EQ(headers.size(), 1u);
  EXPECT_EQ(headers[0].name, "raw.bin");
  EXPECT_EQ(headers[0].size, sizeof(data));

  size_t content_offset = 512;
  EXPECT_EQ(memcmp(tar_content.data() + content_offset, data, sizeof(data)), 0);
}

TEST_F(TarWriterTest, DisableWindows(StreamFileRoundTrip)) {
  const std::string part1 = "Hello, ";
  const std::string part2 = "streamed TAR world!";
  const std::string full_content = part1 + part2;

  {
    TarWriter writer(output_path_);
    auto file_or = writer.StreamFile("streamed.txt", full_content.size());
    ASSERT_OK(file_or.status());
    TarWriter::ScopedFileWriter file = std::move(file_or.value());
    ASSERT_OK(file.Write(part1.data(), part1.size()));
    ASSERT_OK(file.Write(part2.data(), part2.size()));
    ASSERT_OK(file.Finalize());
  }

  std::string tar_content = ReadFile(output_path_);
  auto headers = ParseTarFile(tar_content);

  ASSERT_EQ(headers.size(), 1u);
  EXPECT_EQ(headers[0].name, "streamed.txt");
  EXPECT_EQ(headers[0].size, full_content.size());

  size_t content_offset = 512;
  std::string extracted =
      tar_content.substr(content_offset, full_content.size());
  EXPECT_EQ(extracted, full_content);

  // Header (512) + content (27, padded to 512) + end markers (1024).
  EXPECT_EQ(tar_content.size(), 512u + 512u + 1024u);

  // Padding bytes after the content should be zero.
  for (size_t i = content_offset + full_content.size(); i < 1024; i++) {
    EXPECT_EQ(tar_content[i], '\0')
        << "Padding byte at position " << i << " is not zero";
  }
}

TEST_F(TarWriterTest, DisableWindows(StreamFileAutomaticFinalize)) {
  const std::string content = "no explicit Finalize() call";

  {
    TarWriter writer(output_path_);
    auto file_or = writer.StreamFile("auto.txt", content.size());
    ASSERT_OK(file_or.status());
    TarWriter::ScopedFileWriter file = std::move(file_or.value());
    ASSERT_OK(file.Write(content.data(), content.size()));
    // Destructor of `file` should write the padding.
  }

  std::string tar_content = ReadFile(output_path_);
  auto headers = ParseTarFile(tar_content);

  ASSERT_EQ(headers.size(), 1u);
  EXPECT_EQ(headers[0].name, "auto.txt");
  EXPECT_EQ(headers[0].size, content.size());
}

TEST_F(TarWriterTest, StreamFileRejectsCumulativeOversize) {
  std::vector<uint8_t> buffer;
  TarWriter writer(std::make_unique<BufferTarWriterSink>(&buffer));
  auto file_or = writer.StreamFile("oversize.txt", 5);
  ASSERT_OK(file_or.status());
  TarWriter::ScopedFileWriter file = std::move(file_or.value());

  ASSERT_OK(file.Write("abc", 3));
  auto status = file.Write("def", 3);
  EXPECT_THAT(status, IsError());
  EXPECT_THAT(status.message(), HasSubstr("only 2 bytes remain"));

  // The rejected write poisons the writer, making finalization a no-op.
  EXPECT_OK(file.Finalize());
  EXPECT_OK(writer.Finalize());
}

TEST_F(TarWriterTest, StreamFileRejectsUndersizeOnFinalize) {
  std::vector<uint8_t> buffer;
  TarWriter writer(std::make_unique<BufferTarWriterSink>(&buffer));
  auto file_or = writer.StreamFile("undersize.txt", 5);
  ASSERT_OK(file_or.status());
  TarWriter::ScopedFileWriter file = std::move(file_or.value());

  ASSERT_OK(file.Write("data", 4));
  auto status = file.Finalize();
  EXPECT_THAT(status, IsError());
  EXPECT_THAT(status.message(), HasSubstr("expected 5 bytes"));

  // The incomplete entry poisons the writer, making finalization a no-op.
  EXPECT_OK(writer.Finalize());
}

// Accepts the first `fail_after` bytes, then fails every write.
class FailAfterSink : public TarWriterSink {
 public:
  explicit FailAfterSink(size_t fail_after) : fail_after_(fail_after) {}

  base::Status Write(const void*, size_t len) override {
    if (bytes_seen_ + len > fail_after_) {
      return base::ErrStatus("simulated write failure");
    }
    bytes_seen_ += len;
    return base::OkStatus();
  }

  base::Status WriteFromFd(int, size_t) override {
    return base::ErrStatus("simulated write failure");
  }

 private:
  size_t fail_after_;
  size_t bytes_seen_ = 0;
};

TEST_F(TarWriterTest, CustomSinkAddFilePropagatesError) {
  // Allow the 512-byte header through; fail on the content write.
  TarWriter writer(std::make_unique<FailAfterSink>(512));

  auto status = writer.AddFile("fails.txt", "some content");
  EXPECT_THAT(status, IsError());
  EXPECT_THAT(status.message(), HasSubstr("simulated write failure"));

  // The failed write poisons the writer: finalization becomes a no-op, so
  // the destructor does not crash.
  EXPECT_OK(writer.Finalize());
}

TEST_F(TarWriterTest, CustomSinkStreamFilePropagatesError) {
  TarWriter writer(std::make_unique<FailAfterSink>(512));

  // The header write succeeds, so StreamFile() itself succeeds.
  auto file_or = writer.StreamFile("fails.txt", 5);
  ASSERT_OK(file_or.status());
  TarWriter::ScopedFileWriter file = std::move(file_or.value());
  auto status = file.Write("hello", 5);
  EXPECT_THAT(status, IsError());
  EXPECT_THAT(status.message(), HasSubstr("simulated write failure"));

  // The failed write poisons both the entry and the writer: finalization
  // becomes a no-op at both levels and teardown does not crash.
  EXPECT_OK(file.Finalize());
  EXPECT_OK(writer.Finalize());
}

TEST_F(TarWriterTest, CustomSinkPoisonedTeardownDoesNotCrash) {
  TarWriter writer(std::make_unique<FailAfterSink>(512));
  auto file_or = writer.StreamFile("fails.txt", 5);
  ASSERT_OK(file_or.status());
  TarWriter::ScopedFileWriter file = std::move(file_or.value());
  EXPECT_THAT(file.Write("hello", 5), IsError());
  // No explicit Finalize() anywhere: the failed write poisons the entry and
  // the writer, so both destructors must be no-ops.
}

TEST_F(TarWriterTest, CustomSinkFinalizeErrorPropagates) {
  TarWriter writer(std::make_unique<FailAfterSink>(0));

  EXPECT_THAT(writer.Finalize(), IsError());

  // Idempotent: the second call is a no-op that reports success even though
  // the underlying write never happened; the destructor is likewise a no-op.
  EXPECT_OK(writer.Finalize());
}

}  // namespace
}  // namespace perfetto::trace_processor::util
