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

#include "perfetto/protozero/root_message.h"
#include "test/gtest_and_gmock.h"

namespace protozero {

namespace {
// Like TracePacket, a message type that is declared to be always a root.
class RootOnlyMessage : public Message {};
}  // namespace

template <>
struct IsRootMessage<RootOnlyMessage> : std::true_type {};

namespace {

// The root-ness comes from IsRootMessage and is part of the handle type.
static_assert(!MessageHandle<Message>::kIsRootMessage, "");
static_assert(MessageHandle<RootOnlyMessage>::kIsRootMessage, "");
static_assert(std::is_same<MessageHandle<RootOnlyMessage>,
                           MessageHandle<RootOnlyMessage, true>>::value,
              "");

// Root and non-root handles of the same message type don't convert into each
// other.
static_assert(
    !std::is_convertible<MessageHandle<RootOnlyMessage, true>,
                         MessageHandle<RootOnlyMessage, false>>::value,
    "");
static_assert(!std::is_convertible<MessageHandle<RootOnlyMessage, false>,
                                   MessageHandle<RootOnlyMessage, true>>::value,
              "");

// Move-only.
static_assert(!std::is_copy_constructible<MessageHandle<Message>>::value, "");
static_assert(
    !std::is_copy_constructible<MessageHandle<RootOnlyMessage>>::value,
    "");

TEST(MessageHandleTest, MoveHandleSharedMessageDoesntFinalize) {
  RootMessage<Message> message;

  MessageHandle<Message> handle_1(&message);
  handle_1 = MessageHandle<Message>(&message);
  ASSERT_FALSE(handle_1->is_finalized());
}

TEST(MessageHandleTest, MoveRootHandleSharedMessageDoesntFinalize) {
  RootMessage<RootOnlyMessage> message;

  MessageHandle<RootOnlyMessage> handle_1(&message);
  handle_1 = MessageHandle<RootOnlyMessage>(&message);
  ASSERT_FALSE(handle_1->is_finalized());
}

TEST(MessageHandleTest, RootHandleFinalizesOnDestruction) {
  RootMessage<RootOnlyMessage> message;
  {
    MessageHandle<RootOnlyMessage> handle_1(&message);
    MessageHandle<RootOnlyMessage> handle_2 = std::move(handle_1);
    EXPECT_FALSE(message.is_finalized());
  }
  EXPECT_TRUE(message.is_finalized());
}

}  // namespace
}  // namespace protozero
