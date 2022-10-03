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

#include "perfetto/protozero/message_arena.h"

#include <array>

#include "test/gtest_and_gmock.h"

namespace protozero {
namespace {

using ::testing::NotNull;

TEST(MessageArenaTest, Basic) {
  MessageArena arena;

  Message* msg1 = arena.NewMessage();
  EXPECT_THAT(msg1, NotNull());
  Message* msg2 = arena.NewMessage();
  EXPECT_THAT(msg2, NotNull());
  EXPECT_NE(msg1, msg2);
  arena.DeleteLastMessage(msg2);
  arena.DeleteLastMessage(msg1);

  Message* msg3 = arena.NewMessage();
  EXPECT_THAT(msg3, NotNull());
}

TEST(MessageArenaTest, ManyMessages) {
  MessageArena arena;
  // Ideally this should be more than MessageArena::Block::kCapacity, but that's
  // private.
  constexpr size_t kNumMessages = 32;
  std::array<Message*, kNumMessages> messages;

  for (size_t i = 0; i < kNumMessages; i++) {
    Message* msg = arena.NewMessage();
    EXPECT_THAT(msg, NotNull());
    messages[i] = msg;
  }

  for (auto it = messages.crbegin(); it != messages.crend(); it++) {
    Message* msg = *it;
    arena.DeleteLastMessage(msg);
  }

  Message* msg = arena.NewMessage();
  EXPECT_THAT(msg, NotNull());
}

}  // namespace
}  // namespace protozero
