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

#include "perfetto/protozero/scattered_heap_buffer.h"

#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/protozero/message.h"
#include "test/gtest_and_gmock.h"

namespace protozero {
namespace {

std::vector<uint8_t> Stitch(
    const std::vector<ScatteredHeapBuffer::Slice>& slices) {
  std::vector<uint8_t> out;
  for (const auto& slice : slices) {
    auto range = slice.GetUsedRange();
    out.insert(out.end(), range.begin, range.end);
  }
  return out;
}

std::vector<uint8_t> Serialize(const std::string& payload) {
  HeapBuffered<Message> msg(8, 8);
  msg->AppendString(1, payload);
  return msg.SerializeAsArray();
}

TEST(ScatteredHeapBufferTest, TakeSlicesHandsOverEveryByteWritten) {
  const std::string payload(100, 'x');
  HeapBuffered<Message> msg(8, 8);
  msg->AppendString(1, payload);

  std::vector<ScatteredHeapBuffer::Slice> slices = msg.TakeSlices();
  EXPECT_GT(slices.size(), 1u);
  EXPECT_EQ(Stitch(slices), Serialize(payload));
  EXPECT_TRUE(msg.empty());
}

TEST(ScatteredHeapBufferTest, TakeSlicesLeavesBufferReusable) {
  HeapBuffered<Message> msg(8, 8);
  msg->AppendString(1, "first");
  std::vector<ScatteredHeapBuffer::Slice> first = msg.TakeSlices();

  msg->AppendString(1, "second");
  EXPECT_EQ(msg.SerializeAsArray(), Serialize("second"));

  EXPECT_EQ(Stitch(first), Serialize("first"));
}

TEST(ScatteredHeapBufferTest, TakeSlicesFinalizesNestedMessages) {
  auto write = [](HeapBuffered<Message>& msg) {
    msg->BeginNestedMessage<Message>(1)->AppendString(2, "nested");
  };
  HeapBuffered<Message> expected(64, 64);
  write(expected);

  HeapBuffered<Message> msg(64, 64);
  write(msg);
  std::vector<ScatteredHeapBuffer::Slice> slices = msg.TakeSlices();
  EXPECT_EQ(Stitch(slices), expected.SerializeAsArray());
}

TEST(ScatteredHeapBufferTest, TakeSlicesOnEmptyBuffer) {
  HeapBuffered<Message> msg(8, 8);
  EXPECT_TRUE(msg.TakeSlices().empty());
  EXPECT_TRUE(msg.empty());

  msg->AppendString(1, "after");
  EXPECT_EQ(msg.SerializeAsArray(), Serialize("after"));
}

}  // namespace
}  // namespace protozero
