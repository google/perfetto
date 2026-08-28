/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "perfetto/ext/protozero/proto_ring_buffer.h"

#include <stdint.h>
#include <sys/types.h>

#include <algorithm>
#include <cstring>
#include <list>
#include <ostream>
#include <random>
#include <vector>

#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/proto_utils.h"
#include "test/gtest_and_gmock.h"

using testing::ElementsAre;

namespace protozero {

struct ExpectedMessage {
  const uint8_t* start = nullptr;
  uint32_t len = 0;
  uint32_t field_id = 0;
};

// For ASSERT_EQ()
inline bool operator==(const ProtoRingBuffer::Message& a,
                       const ExpectedMessage& b) {
  if (a.field_id() != b.field_id || a.size() != b.len || !a.valid())
    return false;
  return memcmp(a.data(), b.start, b.len) == 0;
}

inline std::ostream& operator<<(std::ostream& stream,
                                const ExpectedMessage& msg) {
  stream << "Message{field_id:" << msg.field_id << ", len:" << msg.len;
  stream << ", payload: \"";
  static constexpr uint32_t kTruncLen = 16;
  for (uint32_t i = 0; i < std::min(msg.len, kTruncLen); i++)
    stream << static_cast<char>(msg.start[i]);
  if (msg.len > kTruncLen)
    stream << "...";
  stream << "\"}";
  return stream;
}

inline std::ostream& operator<<(std::ostream& stream,
                                const ProtoRingBuffer::Message& msg) {
  return stream << ExpectedMessage{msg.data(), msg.size(), msg.field_id()};
}

namespace {

using ::perfetto::base::ArraySize;

constexpr uint32_t kMaxMsgSize = ProtoRingBuffer::kMaxMsgSize;

void Write(ProtoRingBuffer* buf, const void* data, size_t len) {
  auto handle = buf->BeginWrite(len);
  memcpy(handle.data(), data, len);
  handle.EndWrite(len);
}

class ProtoRingBufferTest : public ::testing::Test {
 public:
  ExpectedMessage MakeProtoMessage(uint32_t field_id,
                                   uint32_t len,
                                   bool append = false) {
    ExpectedMessage msg{};
    namespace proto_utils = protozero::proto_utils;
    const uint8_t* initial_ptr = last_msg_.data();
    if (!append)
      last_msg_.clear();
    size_t initial_size = last_msg_.size();

    // 20 is an over-estimation of the preamble (fixed by the 2nd resize below).
    last_msg_.resize(initial_size + len + 20);
    uint8_t* wptr = &last_msg_[initial_size];
    auto tag = proto_utils::MakeTagLengthDelimited(field_id);
    wptr = proto_utils::WriteVarInt(tag, wptr);
    wptr = proto_utils::WriteVarInt(len, wptr);
    msg.start = wptr;
    msg.len = len;
    msg.field_id = field_id;
    for (uint32_t i = 0; i < len; i++)
      *(wptr++) = '0' + ((len + i) % 73);  // 73 prime for more unique patterns.

    PERFETTO_CHECK(wptr <= &last_msg_.back());
    last_msg_.resize(static_cast<size_t>(wptr - &last_msg_[0]));

    // Vector must not expand, because the returned Mesdage relies on pointer
    // stability. The TEST_F must reserve enough capacity.
    if (append)
      PERFETTO_CHECK(last_msg_.data() == initial_ptr);
    return msg;
  }

  std::vector<uint8_t> last_msg_;
};

TEST_F(ProtoRingBufferTest, CoalescingStream) {
  ProtoRingBuffer buf;
  last_msg_.reserve(1024);
  std::list<ExpectedMessage> expected;

  // Build 6 messages of 100 bytes each (100 does not include preambles).
  for (uint32_t i = 1; i <= 6; i++)
    expected.emplace_back(MakeProtoMessage(i, 100, /*append=*/true));

  uint32_t frag_lens[] = {120, 20, 471, 1};
  uint32_t frag_sum = 0;
  for (uint32_t i = 0; i < ArraySize(frag_lens); i++)
    frag_sum += frag_lens[i];
  ASSERT_EQ(frag_sum, last_msg_.size());

  // Append the messages in such a way that each appen either passes a portion
  // of a message (the 20 ones) or more than a message.
  uint32_t written = 0;
  for (uint32_t i = 0; i < ArraySize(frag_lens); i++) {
    Write(&buf, &last_msg_[written], frag_lens[i]);
    written += frag_lens[i];
    for (;;) {
      auto msg = buf.ReadMessage();
      if (!msg.valid())
        break;
      ASSERT_FALSE(expected.empty());
      ASSERT_EQ(msg, expected.front());
      expected.pop_front();
    }
  }
  EXPECT_TRUE(expected.empty());
}

TEST_F(ProtoRingBufferTest, RandomSizes) {
  ProtoRingBuffer buf;
  std::minstd_rand0 rnd(0);

  last_msg_.reserve(1024 * 1024 * 64);
  std::list<ExpectedMessage> expected;

  const uint32_t kNumMsg = 100;
  for (uint32_t i = 0; i < kNumMsg; i++) {
    uint32_t field_id = static_cast<uint32_t>(1 + (rnd() % 1024u));
    uint32_t rndval = static_cast<uint32_t>(rnd());
    uint32_t len = 1 + (rndval % 1024);
    if ((rndval % 100) < 2) {
      len *= 10 * 1024;  // 2% of messages will get close to kMaxMsgSize
    } else if ((rndval % 100) < 20) {
      len *= 512;  // 18% will be around 500K;
    }
    len = std::max(std::min(len, kMaxMsgSize), 1u);
    expected.push_back(MakeProtoMessage(field_id, len, /*append=*/true));
  }

  uint32_t total = static_cast<uint32_t>(last_msg_.size());
  for (uint32_t frag_sum = 0; frag_sum < total;) {
    uint32_t frag_len = static_cast<uint32_t>(1 + (rnd() % 32768));
    frag_len = std::min(frag_len, total - frag_sum);
    Write(&buf, &last_msg_[frag_sum], frag_len);
    frag_sum += frag_len;
    for (;;) {
      auto msg = buf.ReadMessage();
      if (!msg.valid())
        break;
      ASSERT_FALSE(expected.empty());
      ASSERT_EQ(msg, expected.front());
      expected.pop_front();
    }
  }
  EXPECT_TRUE(expected.empty());
}

TEST_F(ProtoRingBufferTest, HandleProtoErrorsGracefully) {
  ProtoRingBuffer buf;

  // Append a partial valid 32 byte message, followed by some invalild
  // data.
  auto expected = MakeProtoMessage(1, 32);
  Write(&buf, last_msg_.data(), last_msg_.size() - 1);
  auto msg = buf.ReadMessage();
  EXPECT_FALSE(msg.valid());
  EXPECT_FALSE(msg.fatal_framing_error());

  uint8_t invalid[] = {0x7f, 0x7f, 0x7f, 0x7f};
  invalid[0] = last_msg_.back();
  Write(&buf, invalid, sizeof(invalid));

  // The first message should be valild
  msg = buf.ReadMessage();
  EXPECT_EQ(msg, expected);

  // All the rest should be a framing error.
  for (int i = 0; i < 3; i++) {
    msg = buf.ReadMessage();
    EXPECT_FALSE(msg.valid());
    EXPECT_TRUE(msg.fatal_framing_error());

    Write(&buf, invalid, sizeof(invalid));
  }
}

// A read(2) can come back short, so EndWrite() may report fewer bytes than
// BeginWrite() reserved. The tests above always write what they reserve.
TEST_F(ProtoRingBufferTest, PartialWrites) {
  ProtoRingBuffer buf;
  auto expected = MakeProtoMessage(/*field_id=*/7, /*len=*/4096);

  buf.BeginWrite(4096).EndWrite(0);  // The socket had nothing for us.
  EXPECT_FALSE(buf.ReadMessage().valid());

  for (size_t written = 0; written < last_msg_.size();) {
    // Reserve far more than we intend to write, as a socket reader would.
    auto handle = buf.BeginWrite(4096);
    size_t n = std::min<size_t>(37, last_msg_.size() - written);
    memcpy(handle.data(), &last_msg_[written], n);
    handle.EndWrite(n);
    written += n;
    if (written < last_msg_.size())
      ASSERT_FALSE(buf.ReadMessage().valid());
  }
  EXPECT_EQ(buf.ReadMessage(), expected);
}

TEST_F(ProtoRingBufferTest, AbortedWriteKeepsNothing) {
  ProtoRingBuffer buf;
  auto expected = MakeProtoMessage(/*field_id=*/7, /*len=*/32);

  auto aborted = buf.BeginWrite(4096);
  memset(aborted.data(), 0xff, 4096);
  aborted.AbortWrite();
  EXPECT_FALSE(buf.ReadMessage().valid());

  auto handle = buf.BeginWrite(last_msg_.size());
  memcpy(handle.data(), last_msg_.data(), last_msg_.size());
  handle.EndWrite(last_msg_.size());
  EXPECT_EQ(buf.ReadMessage(), expected);
}

TEST_F(ProtoRingBufferTest, MovedHandleCommitsOnce) {
  ProtoRingBuffer buf;
  auto expected = MakeProtoMessage(/*field_id=*/7, /*len=*/32);

  auto handle = buf.BeginWrite(last_msg_.size());
  memcpy(handle.data(), last_msg_.data(), last_msg_.size());
  auto moved = std::move(handle);
  EXPECT_FALSE(static_cast<bool>(handle));
  EXPECT_TRUE(static_cast<bool>(moved));
  moved.EndWrite(last_msg_.size());
  EXPECT_EQ(buf.ReadMessage(), expected);
}

// A stream that can never form a message grows the buffer until the reader
// gives up. That path used to return without recording the reservation size,
// so the caller's EndWrite() tripped a CHECK and took the process down.
TEST_F(ProtoRingBufferTest, GivingUpDoesNotAbort) {
  ProtoRingBuffer buf;
  constexpr size_t kChunk = 64 * 1024 * 1024;
  std::vector<uint8_t> continuation_bytes(kChunk, 0x80);
  for (int i = 0; i < 4; i++) {
    auto handle = buf.BeginWrite(kChunk);
    memcpy(handle.data(), continuation_bytes.data(), kChunk);
    handle.EndWrite(kChunk);
    buf.ReadMessage();
  }
}

TEST_F(ProtoRingBufferTest, SteadyStateDoesNotAllocate) {
  ProtoRingBuffer buf;
  auto expected = MakeProtoMessage(/*field_id=*/1, /*len=*/512);
  for (int i = 0; i < 1000; i++) {
    Write(&buf, last_msg_.data(), last_msg_.size());
    ASSERT_EQ(buf.ReadMessage(), expected);
    ASSERT_FALSE(buf.ReadMessage().valid());
  }
  // The buffer is recycled every time the read cursor catches up with the
  // write one, so half a megabyte never gets past the first allocation.
  EXPECT_EQ(buf.num_buffers_for_testing(), 1u);
}

TEST_F(ProtoRingBufferTest, RetainedMessagesKeepTheirBufferAlive) {
  ProtoRingBuffer buf;
  auto expected = MakeProtoMessage(/*field_id=*/5, /*len=*/100 * 1024);
  std::vector<ProtoRingBuffer::Message> retained;
  for (int i = 0; i < 16; i++) {
    Write(&buf, last_msg_.data(), last_msg_.size());
    retained.push_back(buf.ReadMessage());
    ASSERT_TRUE(retained.back().valid());
  }
  // Messages pinning the buffer push writes onto fresh ones, rather than
  // clobbering payloads the caller is still holding.
  EXPECT_GT(buf.num_buffers_for_testing(), 1u);
  for (const auto& msg : retained)
    EXPECT_EQ(msg, expected);
}

TEST_F(ProtoRingBufferTest, MessageOutlivesTheBuffer) {
  auto expected = MakeProtoMessage(/*field_id=*/3, /*len=*/64);
  ProtoRingBuffer::Message msg;
  {
    ProtoRingBuffer buf;
    Write(&buf, last_msg_.data(), last_msg_.size());
    msg = buf.ReadMessage();
  }
  EXPECT_EQ(msg, expected);
}

TEST_F(ProtoRingBufferTest, RetainingOneMessageRecyclesBuffers) {
  ProtoRingBuffer buf;
  auto expected = MakeProtoMessage(/*field_id=*/9, /*len=*/100 * 1024);
  ProtoRingBuffer::Message inflight;
  for (int i = 0; i < 1000; i++) {
    Write(&buf, last_msg_.data(), last_msg_.size());
    inflight = buf.ReadMessage();  // Releases the previous one.
    ASSERT_EQ(inflight, expected);
  }
  // Each message takes up nearly a whole buffer, so every write has to move
  // onto another one. Handing them off must still not allocate per message.
  EXPECT_EQ(buf.num_buffers_for_testing(), 2u);
}

}  // namespace
}  // namespace protozero
