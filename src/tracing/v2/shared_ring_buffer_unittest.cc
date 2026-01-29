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

#include "src/tracing/v2/shared_ring_buffer.h"

#include <string.h>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

constexpr size_t kRBHeaderSize = SharedRingBuffer::kRingBufferHeaderSize;
constexpr size_t kChunkSize = SharedRingBuffer::kChunkSize;

TEST(SharedRingBufferTest, CreateWriterAndWriteBytes) {
  // Allocate buffer for header + 4 chunks.
  constexpr size_t kNumChunks = 4;
  constexpr size_t kBufSize = kRBHeaderSize + kNumChunks * kChunkSize;
  alignas(8) uint8_t buf[kBufSize] = {};

  SharedRingBuffer rb(buf, kBufSize);
  ASSERT_TRUE(rb.is_valid());
  EXPECT_EQ(rb.start(), buf);
  EXPECT_EQ(rb.size(), kBufSize);
  EXPECT_EQ(rb.num_chunks(), kNumChunks);

  // Create a writer with ID 1.
  auto writer = rb.CreateWriter(/*writer_id=*/1);
  ASSERT_TRUE(writer.is_valid());
  EXPECT_FALSE(writer.is_writing());

  // Write some bytes.
  writer.BeginWrite();
  EXPECT_TRUE(writer.is_writing());

  const char kTestData[] = "hello";
  writer.WriteBytes(kTestData, strlen(kTestData));

  writer.EndWrite();
  EXPECT_FALSE(writer.is_writing());
}

TEST(SharedRingBufferTest, ReaderBasic) {
  constexpr size_t kNumChunks = 4;
  constexpr size_t kBufSize = kRBHeaderSize + kNumChunks * kChunkSize;
  alignas(8) uint8_t buf[kBufSize] = {};

  SharedRingBuffer rb(buf, kBufSize);
  auto writer = rb.CreateWriter(/*writer_id=*/1);
  SharedRingBuffer::Reader reader(&rb);

  // Write a message.
  writer.BeginWrite();
  const char kTestData[] = "hello world";
  writer.WriteBytes(kTestData, strlen(kTestData));
  writer.EndWrite();

  // Read it back.
  EXPECT_TRUE(reader.ReadOneChunk());

  const auto& msgs = reader.completed_messages();
  ASSERT_EQ(msgs.size(), 1u);
  EXPECT_EQ(msgs[0].writer_id, 1);
  EXPECT_EQ(msgs[0].data, "hello world");
}

TEST(SharedRingBufferTest, ReaderMultipleFragmentsInOneChunk) {
  constexpr size_t kNumChunks = 4;
  constexpr size_t kBufSize = kRBHeaderSize + kNumChunks * kChunkSize;
  alignas(8) uint8_t buf[kBufSize] = {};

  SharedRingBuffer rb(buf, kBufSize);
  auto writer = rb.CreateWriter(/*writer_id=*/42);
  SharedRingBuffer::Reader reader(&rb);

  // Write multiple small messages that fit in one chunk.
  writer.BeginWrite();
  writer.WriteBytes("msg1", 4);
  writer.EndWrite();

  writer.BeginWrite();
  writer.WriteBytes("msg2", 4);
  writer.EndWrite();

  writer.BeginWrite();
  writer.WriteBytes("msg3", 4);
  writer.EndWrite();

  // Read the chunk.
  EXPECT_TRUE(reader.ReadOneChunk());

  const auto& msgs = reader.completed_messages();
  ASSERT_EQ(msgs.size(), 3u);
  EXPECT_EQ(msgs[0].data, "msg1");
  EXPECT_EQ(msgs[1].data, "msg2");
  EXPECT_EQ(msgs[2].data, "msg3");
  for (const auto& m : msgs) {
    EXPECT_EQ(m.writer_id, 42);
  }
}

TEST(SharedRingBufferTest, ReaderEmptyBuffer) {
  constexpr size_t kNumChunks = 4;
  constexpr size_t kBufSize = kRBHeaderSize + kNumChunks * kChunkSize;
  alignas(8) uint8_t buf[kBufSize] = {};

  SharedRingBuffer rb(buf, kBufSize);
  SharedRingBuffer::Reader reader(&rb);

  // Reading from empty buffer should return false.
  EXPECT_FALSE(reader.ReadOneChunk());
  EXPECT_TRUE(reader.completed_messages().empty());
}

}  // namespace
}  // namespace perfetto
