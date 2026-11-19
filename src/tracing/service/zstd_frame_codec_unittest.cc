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

#include "src/tracing/service/zstd_frame_codec.h"

#include <optional>
#include <random>
#include <string>
#include <vector>

#include "perfetto/protozero/field.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

// Deterministic input that compresses poorly.
std::string RandomBytes(size_t size, uint32_t seed) {
  std::minstd_rand rnd(seed);
  std::string bytes(size, '\0');
  for (char& c : bytes)
    c = static_cast<char>(rnd());
  return bytes;
}

protozero::ConstBytes AsBytes(const std::string& str) {
  return protozero::ConstBytes{reinterpret_cast<const uint8_t*>(str.data()),
                               str.size()};
}

// Compresses |pieces| into one frame and returns a copy of it.
std::string CompressFrame(ZstdFrameCodec* codec,
                          const std::vector<std::string>& pieces) {
  codec->BeginFrame();
  for (const std::string& piece : pieces)
    codec->AddToFrame(piece.data(), piece.size());
  return codec->EndFrame().ToStdString();
}

// Returns the decoded bytes, or nullopt if decompression fails.
std::optional<std::string> DecompressFrame(ZstdFrameCodec* codec,
                                           const std::string& frame,
                                           size_t expected_size) {
  std::string decoded(expected_size, '\0');
  if (!codec->Decompress(AsBytes(frame),
                         reinterpret_cast<uint8_t*>(decoded.data()),
                         expected_size)) {
    return std::nullopt;
  }
  return decoded;
}

TEST(ZstdFrameCodecTest, RoundTrip) {
  ZstdFrameCodec codec(1024);
  const std::string input(600, 'a');
  const std::string frame = CompressFrame(&codec, {input});
  EXPECT_LT(frame.size(), input.size());
  EXPECT_EQ(DecompressFrame(&codec, frame, input.size()), input);
}

// Alternating gathered and direct input must preserve the order of the bytes.
TEST(ZstdFrameCodecTest, PreservesSmallAndLargeInputOrder) {
  ZstdFrameCodec codec(1024);
  const std::vector<std::string> pieces = {
      "a",
      std::string(100, 'b'),
      "",                     // Empty input is skipped.
      RandomBytes(16384, 1),  // Exactly the size of the gather buffer.
      "c",
      RandomBytes(16385, 2),  // Too large to gather, so zstd gets it directly.
      "end",
  };
  std::string input;
  for (const std::string& piece : pieces)
    input += piece;

  const std::string frame = CompressFrame(&codec, pieces);
  EXPECT_EQ(DecompressFrame(&codec, frame, input.size()), input);
}

TEST(ZstdFrameCodecTest, FramesAreIndependent) {
  ZstdFrameCodec codec(1024);
  const std::string first(500, 'x');
  const std::string second = first + "-second";
  const std::string first_frame = CompressFrame(&codec, {first});
  const std::string second_frame = CompressFrame(&codec, {second});

  // Decode in reverse order with a fresh context to catch any dependency
  // on an earlier frame.
  ZstdFrameCodec decoder(1024);
  EXPECT_EQ(DecompressFrame(&decoder, second_frame, second.size()), second);
  EXPECT_EQ(DecompressFrame(&decoder, first_frame, first.size()), first);
}

TEST(ZstdFrameCodecTest, DecompressRejectsInvalidInputAndSizeMismatch) {
  ZstdFrameCodec codec(1024);
  const std::string input = RandomBytes(2000, 1);
  const std::string frame = CompressFrame(&codec, {input});

  // Reject a valid frame if the caller expects a different decoded size.
  // zstd rejects a smaller |dst|, and Decompress() checks for a larger one.
  EXPECT_EQ(DecompressFrame(&codec, frame, input.size() - 1), std::nullopt);
  EXPECT_EQ(DecompressFrame(&codec, frame, input.size() + 1), std::nullopt);

  EXPECT_EQ(
      DecompressFrame(&codec, frame.substr(0, frame.size() - 1), input.size()),
      std::nullopt);

  // A failed decode must not prevent the next one from succeeding.
  EXPECT_EQ(DecompressFrame(&codec, frame, input.size()), input);
}

// Making room for a new bundle can decompress an old one for the ProtoVMs,
// while TraceBufferV3 still holds the bytes of the new frame.
TEST(ZstdFrameCodecTest, DecompressPreservesFinishedFrame) {
  ZstdFrameCodec codec(1024);
  const std::string old_input = RandomBytes(5000, 1);
  const std::string old_frame = CompressFrame(&codec, {old_input});

  const std::string new_input = RandomBytes(3000, 2);
  codec.BeginFrame();
  codec.AddToFrame(new_input.data(), new_input.size());
  const protozero::ConstBytes new_frame = codec.EndFrame();
  const std::string new_frame_copy = new_frame.ToStdString();

  EXPECT_EQ(DecompressFrame(&codec, old_frame, old_input.size()), old_input);

  EXPECT_EQ(new_frame.ToStdString(), new_frame_copy);
  EXPECT_EQ(DecompressFrame(&codec, new_frame_copy, new_input.size()),
            new_input);
}

// The budget counts the contexts and the initial scratch before first use.
// Oversized scratch must stay counted until BeginFrame() releases it.
TEST(ZstdFrameCodecTest, BeginFrameReleasesOversizedScratch) {
  ZstdFrameCodec codec(1024);
  const size_t initial_memory_usage = codec.GetMemoryUsageBytes();

  const std::string small(1000, 's');
  EXPECT_EQ(
      DecompressFrame(&codec, CompressFrame(&codec, {small}), small.size()),
      small);
  EXPECT_EQ(codec.GetMemoryUsageBytes(), initial_memory_usage);

  // Random bytes do not compress, so the frame grows the scratch above twice
  // its initial capacity, which is what BeginFrame() releases.
  const std::string large = RandomBytes(16 * 1024, 1);
  const std::string large_frame = CompressFrame(&codec, {large});
  EXPECT_EQ(DecompressFrame(&codec, large_frame, large.size()), large);
  EXPECT_GT(codec.GetMemoryUsageBytes(),
            initial_memory_usage + large.size() / 2);

  codec.BeginFrame();
  EXPECT_EQ(codec.GetMemoryUsageBytes(), initial_memory_usage);
  codec.AddToFrame(small.data(), small.size());
  EXPECT_EQ(
      DecompressFrame(&codec, codec.EndFrame().ToStdString(), small.size()),
      small);
}

}  // namespace
}  // namespace perfetto
