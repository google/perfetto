/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "perfetto/protozero/message_handle.h"

#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "test/gtest_and_gmock.h"

namespace protozero {

namespace {

TEST(MessageHandleTest, MoveHandleSharedMessageDoesntFinalize) {
  RootMessage<Message> message;

  MessageHandle<Message> handle_1(&message);
  handle_1 = MessageHandle<Message>(&message);
  ASSERT_FALSE(handle_1->is_finalized());
}

// A handle of the message's own type on a proto group root, as
// TraceWriter::TracePacketHandle is. The handle needs no root type:
// - The root's Finalize() ends the nested message and appends its closing
//   byte.
// - The root gets no closing byte, because it has no parent.
TEST(MessageHandleTest, ProtoGroupRootHandleNeedsNoRootType) {
  ScatteredHeapBuffer buffer;
  ScatteredStreamWriter writer(&buffer);
  buffer.set_writer(&writer);
  RootMessage<Message> root;
  root.Reset(&writer, Message::Encoding::kProtoGroup);
  {
    Message* as_plain_message = &root;
    MessageHandle<Message> handle(as_plain_message);
    handle->BeginNestedMessage<Message>(1)->AppendVarInt(2, 7);
  }
  EXPECT_TRUE(root.is_finalized());
  EXPECT_EQ(buffer.StitchSlices(),
            (std::vector<uint8_t>{0x0b, 0x10, 0x07, 0x04}));
}

}  // namespace
}  // namespace protozero
