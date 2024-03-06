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

#include <list>
#include <ostream>
#include <random>
#include <vector>

#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/proto_utils.h"
#include "test/gtest_and_gmock.h"

using testing::ElementsAre;

namespace protozero {

// For ASSERT_EQ()
inline bool operator==(const ProtoRingBuffer::Message& a,
                       const ProtoRingBuffer::Message& b) {
  if (a.field_id != b.field_id || a.len != b.len || a.valid() != b.valid())
    return false;
  if (!a.valid())
    return true;
  return memcmp(a.start, b.start, a.len) == 0;
}

inline std::ostream& operator<<(std::ostream& stream,
                                const ProtoRingBuffer::Message& msg) {
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

namespace {

using ::perfetto::base::ArraySize;

constexpr uint32_t kMaxMsgSize = ProtoRingBuffer::kMaxMsgSize;

class ProtoRingBufferTest : public ::testing::Test {
 public:
  ProtoRingBuffer::Message MakeProtoMessage(uint32_t field_id,
                                            uint32_t len,
                                            bool append = false) {
    ProtoRingBuffer::Message msg{};
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

// Test that when appending buffers that contain whole messages the ring buffer
// is skipped.
TEST_F(ProtoRingBufferTest, Fastpath) {
  ProtoRingBuffer buf;
  for (uint32_t i = 0; i < 10; i++) {
    // Write a whole message that hits the fastpath.
    auto expected = MakeProtoMessage(/*field_id=*/i + 1, /*len=*/i * 7);
    buf.Append(last_msg_.data(), last_msg_.size());
    // Shouln't take any space the buffer because it hits the fastpath.
    EXPECT_EQ(buf.avail(), buf.capacity());
    auto actual = buf.ReadMessage();
    ASSERT_TRUE(actual.valid());
    EXPECT_EQ(actual.start, expected.start);  // Should point to the same buf.
    EXPECT_EQ(actual, expected);

    // Now write a message in two fragments. It won't hit the fastpath
    expected = MakeProtoMessage(/*field_id*/ 1, /*len=*/32);
    buf.Append(last_msg_.data(), 13);
    EXPECT_LT(buf.avail(), buf.capacity());
    EXPECT_FALSE(buf.ReadMessage().valid());

    // Append 2nd fragment.
    buf.Append(last_msg_.data() + 13, last_msg_.size() - 13);
    actual = buf.ReadMessage();
    ASSERT_TRUE(actual.valid());
    EXPECT_EQ(actual, expected);
  }
}

TEST_F(ProtoRingBufferTest, CoalescingStream) {
  ProtoRingBuffer buf;
  last_msg_.reserve(1024);
  std::list<ProtoRingBuffer::Message> expected;

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
    buf.Append(&last_msg_[written], frag_lens[i]);
    written += frag_lens[i];
    for (;;) {
      auto msg = buf.ReadMessage();
      if (!msg.valid())
        break;
      ASSERT_FALSE(expected.empty());
      ASSERT_EQ(expected.front(), msg);
      expected.pop_front();
    }
  }
  EXPECT_TRUE(expected.empty());
}

TEST_F(ProtoRingBufferTest, RandomSizes) {
  ProtoRingBuffer buf;
  std::minstd_rand0 rnd(0);

  last_msg_.reserve(1024 * 1024 * 64);
  std::list<ProtoRingBuffer::Message> expected;

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
    buf.Append(&last_msg_[frag_sum], frag_len);
    frag_sum += frag_len;
    for (;;) {
      auto msg = buf.ReadMessage();
      if (!msg.valid())
        break;
      ASSERT_FALSE(expected.empty());
      ASSERT_EQ(expected.front(), msg);
      expected.pop_front();
    }
  }
  EXPECT_TRUE(expected.empty());
}

TEST_F(ProtoRingBufferTest, HandleProtoErrorsGracefully) {
  ProtoRingBuffer buf;

  // Apppend a partial valid 32 byte message, followed by some invalild
  // data.
  auto expected = MakeProtoMessage(1, 32);
  buf.Append(last_msg_.data(), last_msg_.size() - 1);
  auto msg = buf.ReadMessage();
  EXPECT_FALSE(msg.valid());
  EXPECT_FALSE(msg.fatal_framing_error);

  uint8_t invalid[] = {0x7f, 0x7f, 0x7f, 0x7f};
  invalid[0] = last_msg_.back();
  buf.Append(invalid, sizeof(invalid));

  // The first message shoudl be valild
  msg = buf.ReadMessage();
  EXPECT_EQ(msg, expected);

  // All the rest should be a framing error.
  for (int i = 0; i < 3; i++) {
    msg = buf.ReadMessage();
    EXPECT_FALSE(msg.valid());
    EXPECT_TRUE(msg.fatal_framing_error);

    buf.Append(invalid, sizeof(invalid));
  }
}

// A customised ring buffer message reader where every message has a
// fixed length of |message_length|.
class FixedLengthRingBuffer final : public RingBufferMessageReader {
 public:
  FixedLengthRingBuffer(size_t message_length)
      : RingBufferMessageReader(), message_length_(message_length) {}

 protected:
  virtual Message TryReadMessage(const uint8_t* start,
                                 const uint8_t* end) override {
    Message msg{};
    if (message_length_ <= static_cast<size_t>(end - start)) {
      msg.start = start;
      msg.len = static_cast<uint32_t>(message_length_);
      msg.field_id = 0;
    }
    return msg;
  }

 private:
  size_t message_length_;
};

TEST(RingBufferTest, FixedLengthRingBuffer) {
  FixedLengthRingBuffer buf(3);
  EXPECT_FALSE(buf.ReadMessage().valid());
  buf.Append("a", 1);
  EXPECT_FALSE(buf.ReadMessage().valid());
  buf.Append("bc", 2);
  FixedLengthRingBuffer::Message msg = buf.ReadMessage();
  EXPECT_TRUE(msg.valid());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(msg.start),
                        static_cast<size_t>(msg.len)),
            "abc");
}

}  // namespace
}  // namespace protozero
