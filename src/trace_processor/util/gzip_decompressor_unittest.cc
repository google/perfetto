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

#include "src/trace_processor/util/gzip_decompressor.h"

#include <zconf.h>
#include <zlib.h>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include "perfetto/base/logging.h"

using std::string;

namespace perfetto::trace_processor::util {

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
  return {output, buffer_len - defstream.avail_out};
}

// Drains all output currently available from `decompressor` into `output`.
static void DrainInto(GzipDecompressor& decompressor, std::string& output) {
  uint8_t buffer[4096];
  for (;;) {
    GzipDecompressor::Result result =
        decompressor.ExtractOutput(buffer, sizeof(buffer));
    if (result.ret != GzipDecompressor::ResultCode::kError) {
      output.append(reinterpret_cast<const char*>(buffer),
                    result.bytes_written);
    }
    if (result.ret != GzipDecompressor::ResultCode::kOk)
      break;
  }
}

// Trivially decompress by feeding the entire input in one shot.
static std::string TrivialDecompress(const std::string& input) {
  GzipDecompressor decompressor;
  decompressor.Feed(reinterpret_cast<const uint8_t*>(input.data()),
                    input.size());
  string output;
  DrainInto(decompressor, output);
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
    decompressor.Feed(reinterpret_cast<const uint8_t*>(buffer),
                      size_t(input.gcount()));
    std::string chunk;
    DrainInto(decompressor, chunk);
    output.write(chunk.data(), std::streamsize(chunk.size()));
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
  GzipDecompressor decompressor;
  const auto* compressed_u8 =
      reinterpret_cast<const uint8_t*>(compressed.data());
  ASSERT_GT(compressed.size(), 17u);
  decompressor.Feed(compressed_u8, 7);
  DrainInto(decompressor, decompressed);
  decompressor.Feed(compressed_u8 + 7, 10);
  DrainInto(decompressor, decompressed);
  decompressor.Feed(compressed_u8 + 17, compressed.size() - 17);
  DrainInto(decompressor, decompressed);

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

}  // namespace perfetto::trace_processor::util
