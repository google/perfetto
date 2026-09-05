/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "perfetto/protozero/message.h"

#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/message_handle.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/root_message.h"
#include "src/base/test/utils.h"
#include "src/protozero/test/fake_scattered_buffer.h"
#include "test/gtest_and_gmock.h"

namespace protozero {

namespace {

constexpr size_t kChunkSize = 16;
constexpr uint8_t kTestBytes[] = {0, 0, 0, 0, 0x42, 1, 0x42, 0xff, 0x42, 0};
constexpr const char kStartWatermark[] = {'a', 'b', 'c', 'd',
                                          '1', '2', '3', '\0'};
constexpr const char kEndWatermark[] = {'9', '8', '7', '6',
                                        'z', 'w', 'y', '\0'};

class FakeRootMessage : public RootMessage<Message> {};
class FakeChildMessage : public Message {};

uint32_t SimpleHash(const std::string& str) {
  uint32_t hash = 5381;
  for (char c : str)
    hash = 33 * hash + static_cast<uint32_t>(c);
  return hash;
}

class MessageTest : public ::testing::Test {
 public:
  void SetUp() override { SetChunkSize(kChunkSize); }

  void TearDown() override {
    // Check that none of the messages created by the text fixtures below did
    // under/overflow their heap boundaries.
    for (std::unique_ptr<uint8_t[]>& mem : messages_) {
      EXPECT_STREQ(kStartWatermark, reinterpret_cast<char*>(mem.get()));
      EXPECT_STREQ(kEndWatermark,
                   reinterpret_cast<char*>(mem.get() + sizeof(kStartWatermark) +
                                           sizeof(FakeRootMessage)));
      FakeRootMessage* msg = reinterpret_cast<FakeRootMessage*>(
          mem.get() + sizeof(kStartWatermark));
      msg->~FakeRootMessage();
      mem.reset();
    }
    messages_.clear();
    stream_writer_.reset();
    buffer_.reset();
  }

  void ResetMessage(FakeRootMessage* msg) { msg->Reset(stream_writer_.get()); }

  void ResetMessage(FakeRootMessage* msg, NestedMessageEncoding encoding) {
    msg->Reset(stream_writer_.get(), encoding);
  }

  void SetChunkSize(size_t size) {
    buffer_.reset(new FakeScatteredBuffer(size));
    stream_writer_.reset(new ScatteredStreamWriter(buffer_.get()));
    chunk_size_ = size;
    readback_pos_ = 0;
  }

  FakeRootMessage* NewMessage() {
    std::unique_ptr<uint8_t[]> mem(
        new uint8_t[sizeof(kStartWatermark) + sizeof(FakeRootMessage) +
                    sizeof(kEndWatermark)]);
    uint8_t* msg_start = mem.get() + sizeof(kStartWatermark);
    memcpy(mem.get(), kStartWatermark, sizeof(kStartWatermark));
    memset(msg_start, 0, sizeof(FakeRootMessage));
    memcpy(msg_start + sizeof(FakeRootMessage), kEndWatermark,
           sizeof(kEndWatermark));
    messages_.push_back(std::move(mem));
    FakeRootMessage* msg = new (msg_start) FakeRootMessage();
    msg->Reset(stream_writer_.get());
    return msg;
  }

  // A root using the tracing v2 proto-group encoding.
  FakeRootMessage* NewProtoGroupMessage() {
    FakeRootMessage* msg = NewMessage();
    msg->Reset(stream_writer_.get(), NestedMessageEncoding::kProtoGroup);
    return msg;
  }

  FakeRootMessage* NewMessageWithSizeField() {
    FakeRootMessage* msg = NewMessage();
    uint8_t* size_field =
        stream_writer_->ReserveBytes(proto_utils::kMessageLengthFieldSize);
    memset(size_field, 0u, proto_utils::kMessageLengthFieldSize);
    msg->set_size_field(size_field);
    return msg;
  }

  size_t GetNumSerializedBytes() {
    if (buffer_->chunks().empty())
      return 0;
    return buffer_->chunks().size() * chunk_size_ -
           stream_writer_->bytes_available();
  }

  std::string GetNextSerializedBytes(size_t num_bytes) {
    size_t old_readback_pos = readback_pos_;
    readback_pos_ += num_bytes;
    return buffer_->GetBytesAsString(old_readback_pos, num_bytes);
  }

  static void BuildNestedMessages(Message* msg,
                                  uint32_t max_depth,
                                  bool empty = false,
                                  uint32_t depth = 0) {
    if (!empty) {
      for (uint32_t i = 1; i <= 128; ++i)
        msg->AppendBytes(i, kTestBytes, sizeof(kTestBytes));
    }

    if (depth < max_depth) {
      auto* nested_msg =
          msg->BeginNestedMessage<FakeChildMessage>(1 + depth * 10);
      BuildNestedMessages(nested_msg, max_depth, empty, depth + 1);
    }

    if (!empty) {
      for (uint32_t i = 129; i <= 256; ++i)
        msg->AppendVarInt(i, 42);
    }

    if ((depth & 2) == 0)
      msg->Finalize();
  }

 private:
  std::unique_ptr<FakeScatteredBuffer> buffer_;
  std::unique_ptr<ScatteredStreamWriter> stream_writer_;
  std::vector<std::unique_ptr<uint8_t[]>> messages_;
  size_t chunk_size_{};
  size_t readback_pos_{};
};

TEST_F(MessageTest, ZeroLengthArraysAndStrings) {
  Message* msg = NewMessage();
  msg->AppendBytes(1 /* field_id */, nullptr, 0);
  msg->AppendString(2 /* field_id */, "");

  EXPECT_EQ(4u, msg->Finalize());
  EXPECT_EQ(4u, GetNumSerializedBytes());

  // These lines match the serialization of the Append* calls above.
  ASSERT_EQ("0A00", GetNextSerializedBytes(2));
  ASSERT_EQ("1200", GetNextSerializedBytes(2));
}

TEST_F(MessageTest, BasicTypesNoNesting) {
  Message* msg = NewMessage();
  msg->AppendVarInt(1 /* field_id */, 0);
  msg->AppendVarInt(2 /* field_id */, std::numeric_limits<uint32_t>::max());
  msg->AppendVarInt(3 /* field_id */, 42);
  msg->AppendVarInt(4 /* field_id */, std::numeric_limits<uint64_t>::max());
  msg->AppendFixed(5 /* field_id */, 3.1415f /* float */);
  msg->AppendFixed(6 /* field_id */, 3.14159265358979323846 /* double */);
  msg->AppendBytes(7 /* field_id */, kTestBytes, sizeof(kTestBytes));

  // Field ids > 16 are expected to be varint encoded (preamble > 1 byte)
  msg->AppendString(257 /* field_id */, "0123456789abcdefABCDEF");
  msg->AppendSignedVarInt(3 /* field_id */, -21);

  EXPECT_EQ(74u, msg->Finalize());
  EXPECT_EQ(74u, GetNumSerializedBytes());

  // These lines match the serialization of the Append* calls above.
  ASSERT_EQ("0800", GetNextSerializedBytes(2));
  ASSERT_EQ("10FFFFFFFF0F", GetNextSerializedBytes(6));
  ASSERT_EQ("182A", GetNextSerializedBytes(2));
  ASSERT_EQ("20FFFFFFFFFFFFFFFFFF01", GetNextSerializedBytes(11));
  ASSERT_EQ("2D560E4940", GetNextSerializedBytes(5));
  ASSERT_EQ("31182D4454FB210940", GetNextSerializedBytes(9));
  ASSERT_EQ("3A0A00000000420142FF4200", GetNextSerializedBytes(12));
  ASSERT_EQ("8A101630313233343536373839616263646566414243444546",
            GetNextSerializedBytes(25));
  ASSERT_EQ("1829", GetNextSerializedBytes(2));
}

TEST_F(MessageTest, NestedMessagesSimple) {
  Message* root_msg = NewMessage();
  root_msg->AppendVarInt(1 /* field_id */, 1);

  FakeChildMessage* nested_msg =
      root_msg->BeginNestedMessage<FakeChildMessage>(128 /* field_id */);
  ASSERT_EQ(0u, reinterpret_cast<uintptr_t>(nested_msg) % sizeof(void*));
  nested_msg->AppendVarInt(2 /* field_id */, 2);

  nested_msg =
      root_msg->BeginNestedMessage<FakeChildMessage>(129 /* field_id */);
  nested_msg->AppendVarInt(4 /* field_id */, 2);

  root_msg->AppendVarInt(5 /* field_id */, 3);

  // The expected size of the root message is supposed to be 14 bytes:
  //   2 bytes for the varint field (id: 1) (1 for preamble and one for payload)
  //   3 bytes for the preamble of the 1st nested message (2 for id, 1 for size)
  //   2 bytes for the varint field (id: 2) of the 1st nested message
  //   3 bytes for the premable of the 2nd nested message
  //   2 bytes for the varint field (id: 4) of the 2nd nested message.
  //   2 bytes for the last varint (id : 5) field of the root message.
  // Test also that finalization is idempontent and Finalize() can be safely
  // called more than once without side effects.
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(14u, root_msg->Finalize());
    EXPECT_EQ(14u, GetNumSerializedBytes());
  }

  ASSERT_EQ("0801", GetNextSerializedBytes(2));

  ASSERT_EQ("820802", GetNextSerializedBytes(3));
  ASSERT_EQ("1002", GetNextSerializedBytes(2));

  ASSERT_EQ("8A0802", GetNextSerializedBytes(3));
  ASSERT_EQ("2002", GetNextSerializedBytes(2));

  ASSERT_EQ("2803", GetNextSerializedBytes(2));
}

// Tests using a AppendScatteredBytes to append raw bytes to
// a message using multiple individual buffers.
TEST_F(MessageTest, AppendScatteredBytes) {
  Message* root_msg = NewMessage();

  uint8_t buffer[42];
  memset(buffer, 0x42, sizeof(buffer));

  ContiguousMemoryRange ranges[] = {{buffer, buffer + sizeof(buffer)},
                                    {buffer, buffer + sizeof(buffer)}};
  root_msg->AppendScatteredBytes(1 /* field_id */, ranges, 2);
  EXPECT_EQ(86u, root_msg->Finalize());
  EXPECT_EQ(86u, GetNumSerializedBytes());

  // field_id
  EXPECT_EQ("0A", GetNextSerializedBytes(1));
  // field length
  EXPECT_EQ("54", GetNextSerializedBytes(1));
  // start of contents
  EXPECT_EQ("42424242", GetNextSerializedBytes(4));
}

TEST_F(MessageTest, AppendRawProtoBytesFinalizesNestedMessage) {
  Message* root_msg = NewMessage();

  uint8_t buffer[42];
  memset(buffer, 0x42, sizeof(buffer));

  FakeChildMessage* nested_msg =
      root_msg->BeginNestedMessage<FakeChildMessage>(9001 /* field_id */);
  nested_msg->AppendVarInt(4 /* field_id */, 2);
  uint8_t* nested_msg_size_field = nested_msg->size_field();

  EXPECT_FALSE(nested_msg->is_finalized());
  EXPECT_EQ(0u, *nested_msg_size_field);

  root_msg->AppendRawProtoBytes(buffer, sizeof(buffer));

  // Nested message should have been finalized as a side effect of appending
  // raw bytes.
  EXPECT_EQ(0x2u, *nested_msg_size_field);
}

TEST_F(MessageTest, AppendScatteredBytesFinalizesNestedMessage) {
  Message* root_msg = NewMessage();

  uint8_t buffer[42];
  memset(buffer, 0x42, sizeof(buffer));

  FakeChildMessage* nested_msg =
      root_msg->BeginNestedMessage<FakeChildMessage>(9001 /* field_id */);
  nested_msg->AppendVarInt(4 /* field_id */, 2);
  uint8_t* nested_msg_size_field = nested_msg->size_field();

  EXPECT_FALSE(nested_msg->is_finalized());
  EXPECT_EQ(0u, *nested_msg_size_field);

  ContiguousMemoryRange ranges[] = {{buffer, buffer + sizeof(buffer)}};
  root_msg->AppendScatteredBytes(1 /* field_id */, ranges, 1);

  // Nested message should have been finalized as a side effect of appending
  // scattered bytes.
  EXPECT_EQ(0x2u, *nested_msg_size_field);
}

TEST_F(MessageTest, StressTest) {
  std::vector<Message*> nested_msgs;

  Message* root_msg = NewMessage();
  BuildNestedMessages(root_msg, /*max_depth=*/10);
  root_msg->Finalize();

  // The main point of this test is to stress the code paths and test for
  // unexpected crashes of the production code. The actual serialization is
  // already covered in the other text fixtures. Keeping just a final smoke test
  // here on the full buffer hash.
  std::string full_buf = GetNextSerializedBytes(GetNumSerializedBytes());
  size_t buf_hash = SimpleHash(full_buf);
  EXPECT_EQ(0xf9e32b65, buf_hash);
}

TEST_F(MessageTest, DeeplyNested) {
  Message* root_msg = NewMessage();
  BuildNestedMessages(root_msg, /*max_depth=*/1000);
  root_msg->Finalize();

  std::string full_buf = GetNextSerializedBytes(GetNumSerializedBytes());
  size_t buf_hash = SimpleHash(full_buf);
  EXPECT_EQ(0xc0fde419, buf_hash);
}

TEST_F(MessageTest, DeeplyNestedEmptyMessages) {
  // Stress test writing deeply nested empty messages, many of which will be
  // packed into the protobuf length field.

  // Use a larger chunk size for this test so there is more opportunity to pack
  // messages.
  SetChunkSize(4096u);

  Message* root_msg = NewMessage();
  BuildNestedMessages(root_msg, /*max_depth=*/1000, /*empty=*/true);
  root_msg->Finalize();

  std::string full_buf = GetNextSerializedBytes(GetNumSerializedBytes());
  size_t buf_hash = SimpleHash(full_buf);
  EXPECT_EQ(0x9371fe8eu, buf_hash);
}

// Release builds must reject an unknown encoding too, hence EXPECT_DEATH
// rather than EXPECT_DCHECK_DEATH.
TEST_F(MessageTest, ResetRejectsUnknownEncoding) {
  FakeRootMessage* msg = NewMessage();
  EXPECT_DEATH_IF_SUPPORTED(
      ResetMessage(msg, static_cast<NestedMessageEncoding>(2)), "");
}

TEST_F(MessageTest, DestructInvalidMessageHandle) {
  FakeRootMessage* msg = NewMessage();
  EXPECT_DCHECK_DEATH({
    MessageHandle<FakeRootMessage> handle(msg);
    ResetMessage(msg);
  });
}

TEST_F(MessageTest, MessageHandle) {
  FakeRootMessage* msg3 = NewMessageWithSizeField();
  FakeRootMessage* msg2 = NewMessageWithSizeField();
  FakeRootMessage* msg1 = NewMessageWithSizeField();
  FakeRootMessage* ignored_msg = NewMessage();
  uint8_t* msg1_size = msg1->size_field();
  uint8_t* msg2_size = msg2->size_field();
  uint8_t* msg3_size = msg3->size_field();

  // Test that the handle going out of scope causes the finalization of the
  // target message and triggers the optional callback.
  {
    MessageHandle<FakeRootMessage> handle1(msg1);
    handle1->AppendBytes(1 /* field_id */, kTestBytes, 1 /* size */);
    ASSERT_EQ(0u, msg1_size[0]);
  }
  ASSERT_EQ(0x3u, msg1_size[0]);

  // Test that the handle can be late initialized.
  MessageHandle<FakeRootMessage> handle2(ignored_msg);
  handle2 = MessageHandle<FakeRootMessage>(msg2);
  handle2->AppendBytes(1 /* field_id */, kTestBytes, 2 /* size */);
  ASSERT_EQ(0u, msg2_size[0]);  // |msg2| should not be finalized yet.

  // Test that std::move works and does NOT cause finalization of the moved
  // message.
  MessageHandle<FakeRootMessage> handle_swp(ignored_msg);
  handle_swp = std::move(handle2);
  ASSERT_EQ(0u, msg2_size[0]);  // msg2 should be NOT finalized yet.
  handle_swp->AppendBytes(2 /* field_id */, kTestBytes, 3 /* size */);

  MessageHandle<FakeRootMessage> handle3(msg3);
  handle3->AppendBytes(1 /* field_id */, kTestBytes, 4 /* size */);
  ASSERT_EQ(0u, msg3_size[0]);  // msg2 should be NOT finalized yet.

  // Both |handle3| and |handle_swp| point to a valid message (respectively,
  // |msg3| and |msg2|). Now move |handle3| into |handle_swp|.
  handle_swp = std::move(handle3);
  ASSERT_EQ(0x89u, msg2_size[0]);  // |msg2| should be finalized at this point.

  // At this point writing into handle_swp should actually write into |msg3|.
  ASSERT_EQ(msg3, &*handle_swp);
  handle_swp->AppendBytes(2 /* field_id */, kTestBytes, 8 /* size */);
  MessageHandle<FakeRootMessage> another_handle(ignored_msg);
  handle_swp = std::move(another_handle);
  ASSERT_EQ(0x90u, msg3_size[0]);  // |msg3| should be finalized at this point.

#if PERFETTO_DCHECK_IS_ON()
  // In developer builds w/ PERFETTO_DCHECK on a finalized message should
  // invalidate the handle, in order to early catch bugs in the client code.
  FakeRootMessage* msg4 = NewMessage();
  MessageHandle<FakeRootMessage> handle4(msg4);
  ASSERT_EQ(msg4, &*handle4);
  msg4->Finalize();
  ASSERT_THAT(handle4.get(), testing::IsNull());
#endif

  // Test also the behavior of handle with non-root (nested) messages.

  uint8_t* size_msg_2;
  {
    auto* nested_msg_1 = NewMessage()->BeginNestedMessage<FakeChildMessage>(3);
    MessageHandle<FakeChildMessage> child_handle_1(nested_msg_1);
    uint8_t* size_msg_1 = nested_msg_1->size_field();
    memset(size_msg_1, 0, proto_utils::kMessageLengthFieldSize);
    child_handle_1->AppendVarInt(1, 0x11);

    auto* nested_msg_2 = NewMessage()->BeginNestedMessage<FakeChildMessage>(2);
    size_msg_2 = nested_msg_2->size_field();
    memset(size_msg_2, 0, proto_utils::kMessageLengthFieldSize);
    MessageHandle<FakeChildMessage> child_handle_2(nested_msg_2);
    child_handle_2->AppendVarInt(2, 0xFF);

    // |nested_msg_1| should not be finalized yet.
    ASSERT_EQ(0u, size_msg_1[0]);

    // This move should cause |nested_msg_1| to be finalized, but not
    // |nested_msg_2|, which will be finalized only after the current scope.
    child_handle_1 = std::move(child_handle_2);
    ASSERT_EQ(0x82u, size_msg_1[0]);
    ASSERT_EQ(0u, size_msg_2[0]);
  }
  ASSERT_EQ(0x3u, size_msg_2[0]);
}

TEST_F(MessageTest, MoveMessageHandle) {
  FakeRootMessage* msg = NewMessageWithSizeField();
  uint8_t* msg_size = msg->size_field();

  // Test that the handle going out of scope causes the finalization of the
  // target message.
  {
    MessageHandle<FakeRootMessage> handle1(msg);
    MessageHandle<FakeRootMessage> handle2{};
    handle1->AppendBytes(1 /* field_id */, kTestBytes, 1 /* size */);
    handle2 = std::move(handle1);
    ASSERT_EQ(0u, msg_size[0]);
  }
  ASSERT_EQ(0x3u, msg_size[0]);
}

TEST_F(MessageTest, FinalizeWithCompaction) {
  FakeRootMessage* msg = NewMessageWithSizeField();

  msg->AppendBytes(1 /* field_id */, kTestBytes, 5 /* size */);
  uint32_t size = msg->Finalize();
  EXPECT_EQ(7u, size);
  EXPECT_EQ(8u, GetNumSerializedBytes());
}

TEST_F(MessageTest, FinalizeWithoutCompaction) {
  FakeRootMessage* msg = NewMessageWithSizeField();

  // This message doesn't fit into a single chunk, so it won't be compacted.
  msg->AppendBytes(1 /* field_id */, kTestBytes, sizeof(kTestBytes) /* size */);
  msg->AppendBytes(1 /* field_id */, kTestBytes, sizeof(kTestBytes) /* size */);
  uint32_t size = msg->Finalize();
  EXPECT_EQ(24u, size);
  EXPECT_EQ(28u, GetNumSerializedBytes());
}

// ---------------------------------------------------------------------------
// Nested-message encodings.
//
// The proto-group encoding is specified at proto_utils::kProtoGroupEndByte.
// Check exact bytes because this is a wire-format contract.
// ---------------------------------------------------------------------------

TEST_F(MessageTest, DefaultEncodingIsLengthDelimited) {
  FakeRootMessage* msg = NewMessage();
  EXPECT_EQ(msg->nested_message_encoding(),
            NestedMessageEncoding::kLengthDelimited);

  Message* nested = msg->BeginNestedMessage<FakeChildMessage>(1);
  EXPECT_EQ(nested->nested_message_encoding(),
            NestedMessageEncoding::kLengthDelimited);
  msg->Finalize();

  // Field 1 as wire type 2 and its length. The empty message fits in one chunk
  // and is short, so the canonical path compacts the four reserved bytes down
  // to one. The proto-group path must not change this behavior.
  EXPECT_EQ(2u, GetNumSerializedBytes());
  EXPECT_EQ("0A00", GetNextSerializedBytes(2));
}

TEST_F(MessageTest, ProtoGroupEmptyNestedMessage) {
  FakeRootMessage* msg = NewProtoGroupMessage();
  msg->BeginNestedMessage<FakeChildMessage>(1);
  msg->Finalize();

  // The worked example: 0b 04.
  EXPECT_EQ(2u, GetNumSerializedBytes());
  EXPECT_EQ("0B04", GetNextSerializedBytes(2));
}

TEST_F(MessageTest, ProtoGroupNestedInsideNested) {
  FakeRootMessage* msg = NewProtoGroupMessage();
  Message* outer = msg->BeginNestedMessage<FakeChildMessage>(1);
  outer->BeginNestedMessage<FakeChildMessage>(2);
  msg->Finalize();

  // The worked example: 0b 13 04 04.
  EXPECT_EQ(4u, GetNumSerializedBytes());
  EXPECT_EQ("0B130404", GetNextSerializedBytes(4));
}

TEST_F(MessageTest, ProtoGroupChildInheritsTheRootEncoding) {
  FakeRootMessage* msg = NewProtoGroupMessage();
  EXPECT_EQ(msg->nested_message_encoding(), NestedMessageEncoding::kProtoGroup);
  Message* child = msg->BeginNestedMessage<FakeChildMessage>(1);
  EXPECT_EQ(child->nested_message_encoding(),
            NestedMessageEncoding::kProtoGroup);
  Message* grandchild = child->BeginNestedMessage<FakeChildMessage>(2);
  EXPECT_EQ(grandchild->nested_message_encoding(),
            NestedMessageEncoding::kProtoGroup);

  // Proto-group messages never reserve a length.
  EXPECT_EQ(nullptr, child->size_field());
  EXPECT_EQ(nullptr, grandchild->size_field());
  msg->Finalize();
}

TEST_F(MessageTest, ProtoGroupRootEmitsNoEndByte) {
  FakeRootMessage* msg = NewProtoGroupMessage();
  msg->AppendVarInt(1, 42);
  msg->Finalize();

  // Just the field: the root's framing belongs to whoever owns the stream.
  EXPECT_EQ(2u, GetNumSerializedBytes());
  EXPECT_EQ("082A", GetNextSerializedBytes(2));
}

TEST_F(MessageTest, ProtoGroupSiblingsAndScalars) {
  FakeRootMessage* msg = NewProtoGroupMessage();
  msg->AppendVarInt(1, 1);
  Message* first = msg->BeginNestedMessage<FakeChildMessage>(2);
  first->AppendVarInt(3, 2);
  Message* second = msg->BeginNestedMessage<FakeChildMessage>(2);
  second->AppendVarInt(3, 3);
  msg->AppendVarInt(4, 4);
  msg->Finalize();

  //  08 01   field 1 = 1
  //  13      start field 2
  //  18 02   field 3 = 2
  //  04      end field 2
  //  13      start field 2
  //  18 03   field 3 = 3
  //  04      end field 2
  //  20 04   field 4 = 4
  EXPECT_EQ(12u, GetNumSerializedBytes());
  EXPECT_EQ("080113180204131803042004", GetNextSerializedBytes(12));
}

TEST_F(MessageTest, ProtoGroupFinalizeIsIdempotent) {
  FakeRootMessage* msg = NewProtoGroupMessage();
  Message* child = msg->BeginNestedMessage<FakeChildMessage>(1);
  EXPECT_EQ(1u, child->Finalize());
  // A second Finalize() must not emit another end byte.
  EXPECT_EQ(1u, child->Finalize());
  EXPECT_EQ(1u, child->Finalize());
  msg->Finalize();
  msg->Finalize();

  EXPECT_EQ(2u, GetNumSerializedBytes());
  EXPECT_EQ("0B04", GetNextSerializedBytes(2));
}

TEST_F(MessageTest, ProtoGroupMultiByteFieldId) {
  FakeRootMessage* msg = NewProtoGroupMessage();
  // (1000 << 3) | 3 = 8003 -> C3 3E.
  msg->BeginNestedMessage<FakeChildMessage>(1000);
  msg->Finalize();

  EXPECT_EQ(3u, GetNumSerializedBytes());
  EXPECT_EQ("C33E04", GetNextSerializedBytes(3));
}

TEST_F(MessageTest, ProtoGroupFramingCrossesChunkBoundaries) {
  // The fixture hands out 16-byte chunks, so a payload this size puts the start
  // tag and the end byte in different chunks.
  FakeRootMessage* msg = NewProtoGroupMessage();
  Message* child = msg->BeginNestedMessage<FakeChildMessage>(1);
  for (uint32_t i = 0; i < 8; ++i)
    child->AppendBytes(2, kTestBytes, sizeof(kTestBytes));
  msg->Finalize();

  // 1 start tag + 8 * (1 tag + 1 length + 10 bytes) + 1 end byte.
  EXPECT_EQ(1u + 8u * 12u + 1u, GetNumSerializedBytes());
  EXPECT_EQ("0B", GetNextSerializedBytes(1));
  for (uint32_t i = 0; i < 8; ++i)
    EXPECT_EQ("120A00000000420142FF4200", GetNextSerializedBytes(12)) << i;
  EXPECT_EQ("04", GetNextSerializedBytes(1));
}

TEST_F(MessageTest, LengthDelimitedCompactionIsUnchanged) {
  // The canonical path still compacts a short single-chunk nested message into
  // a one-byte length; nothing about the new state was allowed to disturb it.
  FakeRootMessage* msg = NewMessageWithSizeField();
  msg->AppendBytes(1, kTestBytes, 5);
  EXPECT_EQ(7u, msg->Finalize());
  EXPECT_EQ(8u, GetNumSerializedBytes());
}

// The new fields must fit in the padding already present in Message.
namespace {
constexpr size_t AlignUp(size_t value, size_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

constexpr size_t ExpectedMessageSize() {
  // stream_writer_, arena_, nested_message_, size_field_.
  size_t size = 4 * sizeof(void*);
  size += sizeof(uint32_t);  // size_
  size += 3;                 // message_state_, encoding, needs_proto_group_end_
#if PERFETTO_DCHECK_IS_ON()
  size = AlignUp(size, alignof(uint32_t));
  size += sizeof(uint32_t);  // generation_
  size = AlignUp(size, sizeof(void*));
  size += sizeof(void*);  // handle_
#endif
  return AlignUp(size, alignof(Message));
}
}  // namespace

static_assert(sizeof(Message) == ExpectedMessageSize(),
              "protozero::Message grew: the nested-message encoding and the "
              "proto-group flag must fit in existing padding");
static_assert(alignof(Message) == alignof(void*),
              "protozero::Message must stay pointer-aligned");

}  // namespace
}  // namespace protozero
