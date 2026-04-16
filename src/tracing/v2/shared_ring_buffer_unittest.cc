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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

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

// Multi-threaded stress test to verify CAS correctness.
// Multiple writers write variable-length messages with sequence numbers.
// A reader on the main thread verifies monotonicity and payload integrity.
TEST(SharedRingBufferTest, MultiThreadedStressTest) {
  constexpr size_t kNumWriters = 128;
  constexpr size_t kIterationsPerWriter = 100000;
  constexpr size_t kMinMsgSize = 4;  // Minimum: just the sequence number.
  // Use small messages (< chunk payload 252) to avoid fragmentation for now.
  constexpr size_t kMaxMsgSize = 8192;
  constexpr uint32_t kMaxSleepUs = 100;

  constexpr size_t kNumChunks = 512;
  constexpr size_t kBufSize =
      SharedRingBuffer::kRingBufferHeaderSize + kNumChunks * kChunkSize;
  std::unique_ptr<uint8_t[]> buf(new uint8_t[kBufSize]());
  memset(buf.get(), 0, kBufSize);

  SharedRingBuffer rb(buf.get(), kBufSize);
  ASSERT_TRUE(rb.is_valid());

  // State shared between writers and the reader.
  std::atomic<uint32_t> writers_finished{0};

  // --- Writer thread function ---
  auto writer_fn = [&](WriterID writer_id, uint64_t seed) {
    auto writer = rb.CreateWriter(writer_id);
    PERFETTO_CHECK(writer.is_valid());

    // Simple LCG for deterministic pseudo-random numbers.
    auto next_rand = [&seed]() -> uint32_t {
      seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
      return static_cast<uint32_t>(seed >> 32);
    };

    std::vector<uint8_t> msg_buf(kMaxMsgSize);

    for (uint32_t seq = 0; seq < kIterationsPerWriter; ++seq) {
      // Generate a random message size.
      size_t msg_size =
          kMinMsgSize + (next_rand() % (kMaxMsgSize - kMinMsgSize + 1));

      // First 4 bytes: sequence number (little-endian).
      memcpy(msg_buf.data(), &seq, sizeof(seq));

      // Payload: deterministic function of sequence number and writer_id.
      // Use a simple hash: payload[i] = (seq + i + writer_id) & 0xFF.
      for (size_t i = 4; i < msg_size; ++i) {
        msg_buf[i] = static_cast<uint8_t>((seq + i + writer_id) & 0xFF);
      }

      writer.BeginWrite();
      writer.WriteBytes(msg_buf.data(), msg_size);
      writer.EndWrite();

      // Random sleep 0-100us. 0 means no sleep.
      uint32_t sleep_us = next_rand() % (kMaxSleepUs + 1);
      if (sleep_us > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
      }
    }

    writers_finished.fetch_add(1, std::memory_order_release);
  };

  // --- Launch writer threads ---
  std::vector<std::thread> writer_threads;
  for (size_t i = 0; i < kNumWriters; ++i) {
    WriterID writer_id = static_cast<WriterID>(i + 1);
    uint64_t seed = 12345 + i * 9999;
    writer_threads.emplace_back(writer_fn, writer_id, seed);
  }

  // --- Reader on main thread ---
  SharedRingBuffer::Reader reader(&rb);

  // Track the last seen sequence number for each writer (for monotonicity).
  std::vector<int64_t> last_seq(kNumWriters, -1);
  // Track message counts and data losses per writer.
  std::vector<uint64_t> msg_counts(kNumWriters, 0);
  std::vector<uint64_t> gaps_detected(kNumWriters, 0);
  std::vector<uint64_t> payload_errors(kNumWriters, 0);
  std::vector<uint64_t> reorderings(kNumWriters, 0);
  std::vector<uint64_t> bad_writer_ids(1, 0);
  std::vector<uint64_t> short_messages(1, 0);

  // Simple LCG for reader's random sleep.
  uint64_t reader_seed = 54321;
  auto reader_rand = [&reader_seed]() -> uint32_t {
    reader_seed = reader_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<uint32_t>(reader_seed >> 32);
  };

  // Keep reading until all writers are done AND buffer is drained.
  while (true) {
    bool chunk_read = reader.ReadOneChunk();

    if (chunk_read) {
      // Process all completed messages.
      for (const auto& msg : reader.completed_messages()) {
        WriterID wid = msg.writer_id;

        // Validate writer ID.
        if (wid < 1 || wid > kNumWriters) {
          bad_writer_ids[0]++;
          continue;
        }
        size_t writer_idx = wid - 1;

        // Message must be at least 4 bytes (sequence number).
        if (msg.data.size() < 4) {
          short_messages[0]++;
          continue;
        }

        // Extract sequence number.
        uint32_t seq;
        memcpy(&seq, msg.data.data(), sizeof(seq));

        // Check monotonicity: seq must be > last_seq[writer_idx].
        int64_t last = last_seq[writer_idx];
        if (static_cast<int64_t>(seq) <= last) {
          reorderings[writer_idx]++;
          continue;  // Skip updating last_seq to allow recovery.
        }

        // Check for gaps (data loss).
        if (last >= 0 && static_cast<int64_t>(seq) != last + 1) {
          uint64_t lost = seq - static_cast<uint32_t>(last) - 1;
          gaps_detected[writer_idx] += lost;
        }

        last_seq[writer_idx] = static_cast<int64_t>(seq);
        msg_counts[writer_idx]++;

        // Verify payload integrity.
        bool payload_ok = true;
        for (size_t i = 4; i < msg.data.size(); ++i) {
          uint8_t expected = static_cast<uint8_t>((seq + i + wid) & 0xFF);
          if (static_cast<uint8_t>(msg.data[i]) != expected) {
            payload_ok = false;
            break;
          }
        }
        if (!payload_ok) {
          payload_errors[writer_idx]++;
        }
      }
      reader.ClearCompletedMessages();
    } else {
      // No chunk available. Check if all writers are done.
      if (writers_finished.load(std::memory_order_acquire) == kNumWriters) {
        // Do one more read attempt to drain any remaining data.
        bool more = reader.ReadOneChunk();
        if (!more) {
          break;  // Done.
        }
        // If we got more data, process it in the next iteration.
        continue;
      }

      // Sleep for a random amount of time.
      uint32_t sleep_us = reader_rand() % (kMaxSleepUs + 1);
      if (sleep_us > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
      }
    }
  }

  // Wait for all writer threads to finish.
  for (auto& t : writer_threads) {
    t.join();
  }

  // --- Print report ---
  printf("\n=== Multi-threaded Stress Test Report ===\n");
  printf("Iterations per writer: %zu\n", kIterationsPerWriter);
  printf("Bad writer IDs: %lu\n",
         static_cast<unsigned long>(bad_writer_ids[0]));
  printf("Short messages: %lu\n",
         static_cast<unsigned long>(short_messages[0]));

  uint64_t total_received = 0;
  uint64_t total_lost = 0;
  uint64_t total_reorderings = 0;
  uint64_t total_payload_errors = 0;
  for (size_t i = 0; i < kNumWriters; ++i) {
    WriterID wid = static_cast<WriterID>(i + 1);
    printf(
        "Writer %u: received=%lu, gaps=%lu, data_losses=%u, reorderings=%lu, "
        "payload_errors=%lu\n",
        wid, static_cast<unsigned long>(msg_counts[i]),
        static_cast<unsigned long>(gaps_detected[i]),
        reader.GetDataLossesForWriter(wid),
        static_cast<unsigned long>(reorderings[i]),
        static_cast<unsigned long>(payload_errors[i]));
    total_received += msg_counts[i];
    total_lost += gaps_detected[i];
    total_reorderings += reorderings[i];
    total_payload_errors += payload_errors[i];
  }
  printf("Total: received=%lu, lost=%lu (%.2f%% loss rate)\n",
         static_cast<unsigned long>(total_received),
         static_cast<unsigned long>(total_lost),
         total_lost > 0 ? 100.0 * static_cast<double>(total_lost) /
                              static_cast<double>(total_received + total_lost)
                        : 0.0);
  printf("Ring buffer data_losses counter: %u\n",
         rb.header()->data_losses.load(std::memory_order_relaxed));
  printf("==========================================\n\n");

  // Assertions at the end after all threads joined.
  EXPECT_EQ(bad_writer_ids[0], 0u) << "Invalid writer IDs detected";
  EXPECT_EQ(short_messages[0], 0u) << "Messages shorter than 4 bytes detected";
  EXPECT_EQ(total_reorderings, 0u) << "Sequence reorderings detected";
  EXPECT_EQ(total_payload_errors, 0u) << "Payload corruption detected";
}

}  // namespace
}  // namespace perfetto
