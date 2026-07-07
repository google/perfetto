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

#include "src/trace_processor/util/zstd_decompressor.h"

#include "src/trace_processor/util/decompressor.h"

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <zstd.h>

#include "perfetto/base/logging.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::util {
namespace {

std::vector<uint8_t> Compress(const std::string& input) {
  size_t bound = ZSTD_compressBound(input.size());
  std::vector<uint8_t> out(bound);
  size_t sz = ZSTD_compress(out.data(), out.size(), input.data(), input.size(),
                            /*level=*/3);
  PERFETTO_CHECK(!ZSTD_isError(sz));
  out.resize(sz);
  return out;
}

std::string RandomString(size_t size) {
  std::default_random_engine rnd(0);
  std::uniform_int_distribution<> dist(0, 255);
  std::string s(size, '\0');
  for (char& c : s)
    c = static_cast<char>(dist(rnd));
  return s;
}

std::string ToString(const DecompressedBuffer& buf) {
  return std::string(reinterpret_cast<const char*>(buf.data.get()), buf.size);
}

TEST(ZstdUtilsTest, DecompressToBufferRoundTrip) {
  std::string input = "the quick brown fox jumps over the lazy dog";
  std::vector<uint8_t> compressed = Compress(input);

  std::optional<DecompressedBuffer> out = DecompressToBuffer(
      CompressionType::kZstd, compressed.data(), compressed.size());
  ASSERT_TRUE(out);
  EXPECT_EQ(ToString(*out), input);
}

TEST(ZstdUtilsTest, DecompressToBufferEmpty) {
  std::vector<uint8_t> compressed = Compress("");
  std::optional<DecompressedBuffer> out = DecompressToBuffer(
      CompressionType::kZstd, compressed.data(), compressed.size());
  ASSERT_TRUE(out);
  EXPECT_EQ(out->size, 0u);
}

TEST(ZstdUtilsTest, DecompressToBufferCorrupt) {
  // Valid zstd magic followed by garbage.
  std::vector<uint8_t> bad = {0x28, 0xB5, 0x2F, 0xFD, 0x01, 0x02, 0x03, 0x04};
  std::optional<DecompressedBuffer> out =
      DecompressToBuffer(CompressionType::kZstd, bad.data(), bad.size());
  EXPECT_FALSE(out);
}

TEST(ZstdUtilsTest, DecompressToBufferMultiFrame) {
  // Concatenated same-codec frames (e.g. pzstd output) must decode to the end,
  // not stop after the first frame.
  std::vector<uint8_t> compressed = Compress("frame-one");
  std::vector<uint8_t> second = Compress("frame-two");
  compressed.insert(compressed.end(), second.begin(), second.end());

  std::optional<DecompressedBuffer> out = DecompressToBuffer(
      CompressionType::kZstd, compressed.data(), compressed.size());
  ASSERT_TRUE(out);
  EXPECT_EQ(ToString(*out), "frame-oneframe-two");
}

TEST(ZstdUtilsTest, DecompressToBufferTruncated) {
  // A stream cut mid-frame must yield nothing, not silent partial data.
  std::string input = RandomString(64 * 1024);
  std::vector<uint8_t> compressed = Compress(input);
  compressed.resize(compressed.size() / 2);

  std::optional<DecompressedBuffer> out = DecompressToBuffer(
      CompressionType::kZstd, compressed.data(), compressed.size());
  EXPECT_FALSE(out);
}

// Drives the streaming path with a tiny output buffer so the internal zstd
// block buffer must be flushed across multiple ExtractOutput() calls. This is
// the case the whole-file DecompressingTraceReader relies on.
TEST(ZstdUtilsTest, StreamingSmallOutputBuffer) {
  std::string input = RandomString(200 * 1024);
  std::vector<uint8_t> compressed = Compress(input);

  ZstdDecompressor decompressor;
  std::vector<uint8_t> out;
  decompressor.Feed(compressed.data(), compressed.size());
  ZstdDecompressor::Result result;
  do {
    uint8_t buf[512];
    result = decompressor.ExtractOutput(buf, sizeof(buf));
    ASSERT_NE(result.ret, ZstdDecompressor::ResultCode::kError);
    out.insert(out.end(), buf, buf + result.bytes_written);
  } while (result.ret == ZstdDecompressor::ResultCode::kOk);

  EXPECT_EQ(result.ret, ZstdDecompressor::ResultCode::kEof);
  ASSERT_EQ(out.size(), input.size());
  EXPECT_EQ(std::string(out.begin(), out.end()), input);
}

TEST(ZstdUtilsTest, MultiFrame) {
  std::vector<uint8_t> compressed = Compress("frame-one");
  std::vector<uint8_t> second = Compress("frame-two");
  compressed.insert(compressed.end(), second.begin(), second.end());

  ZstdDecompressor decompressor;
  decompressor.Feed(compressed.data(), compressed.size());
  std::string out;
  for (;;) {
    uint8_t buf[1024];
    auto result = decompressor.ExtractOutput(buf, sizeof(buf));
    ASSERT_NE(result.ret, ZstdDecompressor::ResultCode::kError);
    out.append(reinterpret_cast<char*>(buf), result.bytes_written);
    if (result.ret == ZstdDecompressor::ResultCode::kEof) {
      decompressor.Reset();
      if (decompressor.AvailIn() == 0)
        break;
    } else if (result.ret == ZstdDecompressor::ResultCode::kNeedsMoreInput) {
      break;
    }
  }
  EXPECT_EQ(out, "frame-oneframe-two");
}

}  // namespace
}  // namespace perfetto::trace_processor::util
