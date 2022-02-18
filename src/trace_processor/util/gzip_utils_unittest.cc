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

#include "test/gtest_and_gmock.h"

#include "src/trace_processor/util/gzip_utils.h"

#include <zlib.h>
#include <fstream>
#include <iostream>
#include "perfetto/base/logging.h"

using std::string;

namespace perfetto {
namespace trace_processor {
namespace util {

static std::string TrivialGzipCompress(const std::string& input) {
  constexpr auto buffer_len = 10000;
  std::unique_ptr<char[]> output_ptr(new char[buffer_len]);
  char* output = output_ptr.get();
  z_stream defstream;
  defstream.zalloc = Z_NULL;
  defstream.zfree = Z_NULL;
  defstream.opaque = Z_NULL;
  defstream.avail_in = uint32_t(input.size());
  defstream.next_in =
      const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
  defstream.avail_out = buffer_len;
  defstream.next_out = reinterpret_cast<Bytef*>(output);
  deflateInit(&defstream, Z_BEST_COMPRESSION);  // GZip decompress
  deflate(&defstream, Z_FINISH);
  deflateEnd(&defstream);
  PERFETTO_CHECK(defstream.avail_out > 0);
  return std::string(output, buffer_len - defstream.avail_out);
}

// Trivially decompress using ZlibOnlineDecompress.
// It's called 'trivial' because we are feeding the entire input in one shot.
static std::string TrivialDecompress(const std::string& input) {
  string output;
  GzipDecompressor decompressor;
  decompressor.FeedAndExtract(
      reinterpret_cast<const uint8_t*>(input.data()), uint32_t(input.size()),
      [&](const uint8_t* data, size_t len) {
        output.append(reinterpret_cast<const char*>(data), len);
      });
  return output;
}

// Decompress a large GZip file using a in-memory buffer of 4KB, and write the
// decompressed output in another file.
static void DecompressGzipFileInFileOut(const std::string& input_file,
                                        const std::string& output_file) {
  std::ofstream output(output_file.c_str(), std::ios::out | std::ios::binary);
  std::ifstream input(input_file.c_str(), std::ios::binary);
  GzipDecompressor decompressor;
  constexpr uint32_t buffer_sizeof = 4096;
  char buffer[buffer_sizeof];
  while (!input.eof()) {
    input.read(buffer, buffer_sizeof);
    decompressor.FeedAndExtract(
        reinterpret_cast<const uint8_t*>(buffer), size_t(input.gcount()),
        [&](const uint8_t* data, size_t len) {
          output.write(reinterpret_cast<const char*>(data),
                       std::streamsize(len));
        });
  }
  EXPECT_FALSE(input.bad());
}

TEST(GzipDecompressor, Basic) {
  string input = "Abc..Def..Ghi";
  string compressed = TrivialGzipCompress(input);
  EXPECT_EQ(21u, compressed.size());
  string decompressed = TrivialDecompress(compressed);
  EXPECT_EQ(input, decompressed);
}

TEST(GzipDecompressor, Streaming) {
  string input = "Abc..Def..Ghi";
  string compressed = TrivialGzipCompress(input);
  string decompressed;
  auto consumer = [&](const uint8_t* data, size_t len) {
    decompressed.append(reinterpret_cast<const char*>(data), len);
  };
  GzipDecompressor decompressor;
  auto compressed_u8 = reinterpret_cast<const uint8_t*>(compressed.data());
  ASSERT_GT(compressed.size(), 17u);
  decompressor.FeedAndExtract(compressed_u8, 7, consumer);
  decompressor.FeedAndExtract(compressed_u8 + 7, 10, consumer);
  decompressor.FeedAndExtract(compressed_u8 + 17, compressed.size() - 17,
                              consumer);

  EXPECT_EQ(input, decompressed);
}

static std::string ReadFile(const std::string& file_name) {
  std::ifstream fd(file_name, std::ios::binary);
  std::stringstream buffer;
  buffer << fd.rdbuf();
  fd.close();
  return buffer.str();
}

static void WriteFile(const std::string& file_name,
                      const std::string& content) {
  std::ofstream fd(file_name, std::ios::out | std::ios::binary);
  fd.write(content.data(), std::streamsize(content.size()));
  fd.close();
}

TEST(GzipDecompressor, DISABLED_FileInFileOut) {
  auto big_string = []() {
    std::string output;
    for (int i = 0; i < 1000; i++) {
      output += "Abc..Def..Ghi.";  // len = 14
    }
    return output;
  }();
  constexpr auto gz_file = "/tmp/abc.gz";
  constexpr auto txt_file = "/tmp/abc.txt";
  EXPECT_EQ(size_t(1000 * 14), big_string.size());
  WriteFile(gz_file, TrivialGzipCompress(big_string));
  DecompressGzipFileInFileOut(gz_file, txt_file);
  EXPECT_TRUE(ReadFile(txt_file) == big_string);
}

}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto
