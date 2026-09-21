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

#include <type_traits>
#include <utility>

#include "perfetto/protozero/root_message.h"
#include "src/base/test/utils.h"
#include "src/protozero/test/fake_scattered_buffer.h"
#include "test/gtest_and_gmock.h"

namespace protozero {

namespace {
// Like TracePacket, a message type that is declared to be always a root.
class RootOnlyMessage : public Message {};
}  // namespace

template <>
struct IsRootMessage<RootOnlyMessage> : std::true_type {};

namespace {

static_assert(!MessageHandle<Message>::kIsRootMessage, "");
static_assert(MessageHandle<RootOnlyMessage>::kIsRootMessage, "");

static_assert(!std::is_copy_constructible<MessageHandle<Message>>::value, "");
static_assert(
    !std::is_copy_constructible<MessageHandle<RootOnlyMessage>>::value,
    "");
static_assert(sizeof(MessageHandle<Message>) == 2 * sizeof(void*), "");
static_assert(sizeof(MessageHandle<RootOnlyMessage>) == 2 * sizeof(void*), "");

class MockFinalizationListener : public MessageFinalizationListener {
 public:
  MOCK_METHOD(void, OnMessageFinalized, (Message*), (override));
};

template <typename T>
class MessageHandleTypedTest : public testing::Test {
 protected:
  using Handle = MessageHandle<T>;
  RootMessage<T> message_;
  testing::StrictMock<MockFinalizationListener> listener_;
};

using MessageTypes = testing::Types<Message, RootOnlyMessage>;
TYPED_TEST_SUITE(MessageHandleTypedTest, MessageTypes);

TYPED_TEST(MessageHandleTypedTest, EmptyHandle) {
  typename TestFixture::Handle handle;
  handle.set_finalization_listener(&this->listener_);
  EXPECT_FALSE(handle);
  EXPECT_EQ(handle.get(), nullptr);
}

TYPED_TEST(MessageHandleTypedTest, MoveConstructionTransfersListener) {
  using Handle = typename TestFixture::Handle;
  EXPECT_CALL(this->listener_, OnMessageFinalized(&this->message_))
      .WillOnce([](Message* message) { EXPECT_TRUE(message->is_finalized()); });
  {
    Handle source(&this->message_);
    source.set_finalization_listener(&this->listener_);
    Handle destination(std::move(source));
    EXPECT_FALSE(source);
    EXPECT_EQ(destination.get(), &this->message_);
    EXPECT_FALSE(this->message_.is_finalized());
  }
  EXPECT_TRUE(this->message_.is_finalized());
}

TYPED_TEST(MessageHandleTypedTest, MoveAssignmentFinalizesOldMessage) {
  using Handle = typename TestFixture::Handle;
  RootMessage<TypeParam> other;
  testing::StrictMock<MockFinalizationListener> other_listener;
  testing::InSequence sequence;
  EXPECT_CALL(this->listener_, OnMessageFinalized(&this->message_));
  EXPECT_CALL(other_listener, OnMessageFinalized(&other));
  {
    Handle destination(&this->message_);
    destination.set_finalization_listener(&this->listener_);
    Handle source(&other);
    source.set_finalization_listener(&other_listener);
    destination = std::move(source);
    EXPECT_TRUE(this->message_.is_finalized());
    EXPECT_FALSE(other.is_finalized());
    EXPECT_FALSE(source);
    EXPECT_EQ(destination.get(), &other);
  }
  EXPECT_TRUE(other.is_finalized());
}

TYPED_TEST(MessageHandleTypedTest,
           MoveAssignmentOfSameMessageTransfersListener) {
  using Handle = typename TestFixture::Handle;
  testing::StrictMock<MockFinalizationListener> old_listener;
  EXPECT_CALL(this->listener_, OnMessageFinalized(&this->message_));
  {
    Handle destination(&this->message_);
    destination.set_finalization_listener(&old_listener);
    Handle source(&this->message_);
    source.set_finalization_listener(&this->listener_);
    destination = std::move(source);
    EXPECT_FALSE(source);
    EXPECT_FALSE(this->message_.is_finalized());
  }
}

TYPED_TEST(MessageHandleTypedTest, SelfMoveKeepsMessageAndListener) {
  using Handle = typename TestFixture::Handle;
  EXPECT_CALL(this->listener_, OnMessageFinalized(&this->message_));
  {
    Handle handle(&this->message_);
    handle.set_finalization_listener(&this->listener_);
    auto& same_handle = handle;
    handle = std::move(same_handle);
    EXPECT_EQ(handle.get(), &this->message_);
    EXPECT_FALSE(this->message_.is_finalized());
  }
  EXPECT_TRUE(this->message_.is_finalized());
}

TYPED_TEST(MessageHandleTypedTest, DirectFinalizeKeepsHandleAndListener) {
  using Handle = typename TestFixture::Handle;
  {
    Handle handle(&this->message_);
    handle.set_finalization_listener(&this->listener_);
    EXPECT_EQ(handle->Finalize(), 0u);
    EXPECT_EQ(handle->Finalize(), 0u);
    EXPECT_EQ(handle.get(), &this->message_);
    EXPECT_TRUE(handle);
    // Expect the callback only when the handle is destroyed.
    EXPECT_CALL(this->listener_, OnMessageFinalized(&this->message_));
  }
}

TYPED_TEST(MessageHandleTypedTest, ResetHandleBeforeReusingMessage) {
  using Handle = typename TestFixture::Handle;
  Handle handle(&this->message_);
  EXPECT_CALL(this->listener_, OnMessageFinalized(&this->message_));
  handle.set_finalization_listener(&this->listener_);
  handle = Handle();
  EXPECT_FALSE(handle);
  EXPECT_TRUE(this->message_.is_finalized());

  this->message_.Reset(nullptr);
  handle = Handle(&this->message_);
  EXPECT_FALSE(this->message_.is_finalized());
  handle = Handle();
  EXPECT_TRUE(this->message_.is_finalized());
}

TYPED_TEST(MessageHandleTypedTest, TakeStreamWriterClearsHandleAndListener) {
  using Handle = typename TestFixture::Handle;
  FakeScatteredBuffer buffer(32);
  ScatteredStreamWriter writer(&buffer);
  this->message_.Reset(&writer);
  {
    Handle handle(&this->message_);
    handle.set_finalization_listener(&this->listener_);
    EXPECT_EQ(handle.TakeStreamWriter(), &writer);
    EXPECT_FALSE(handle);
    writer.WriteByte(0x08);
    writer.WriteByte(0x01);
  }
  EXPECT_FALSE(this->message_.is_finalized());
  EXPECT_EQ(buffer.GetBytesAsString(0, 2), "0801");
}

TYPED_TEST(MessageHandleTypedTest, AppendAfterFinalizeFailsInDebug) {
  using Handle = typename TestFixture::Handle;
  FakeScatteredBuffer buffer(32);
  ScatteredStreamWriter writer(&buffer);
  this->message_.Reset(&writer);
  Handle handle(&this->message_);
  handle->Finalize();
  EXPECT_DCHECK_DEATH({ handle->AppendVarInt(1, 1); });
}

// Root-ness is part of the handle type. A root handle and a plain handle of
// the same message type are different types.
static_assert(!std::is_same<MessageHandle<RootOnlyMessage>,
                            MessageHandle<RootOnlyMessage, false>>::value,
              "");
static_assert(
    std::is_same<MessageHandle<Message>, MessageHandle<Message, false>>::value,
    "");

TEST(MessageHandleTest, NestedHandleEndsBeforeArenaStorageIsReused) {
  FakeScatteredBuffer buffer(32);
  ScatteredStreamWriter writer(&buffer);
  RootMessage<> root;
  root.Reset(&writer);
  Message* first;
  {
    first = root.BeginNestedMessage<Message>(1);
    MessageHandle<Message> handle(first);
    handle->AppendVarInt(1, 1);
  }
  auto* second = root.BeginNestedMessage<Message>(1);
  EXPECT_EQ(first, second);
  {
    MessageHandle<Message> handle(second);
    handle->AppendVarInt(1, 2);
  }
  EXPECT_EQ(root.Finalize(), 8u);
  EXPECT_EQ(buffer.GetBytesAsString(0, 8), "0A0208010A020802");
}

}  // namespace
}  // namespace protozero
