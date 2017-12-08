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

#include "perfetto/tracing/core/trace_packet.h"

#include <string>

#include "gtest/gtest.h"

#include "protos/trace_packet.pb.h"

namespace perfetto {
namespace {

TEST(TracePacketTest, Simple) {
  protos::TracePacket proto;
  proto.set_test("string field");
  std::string ser_buf = proto.SerializeAsString();
  TracePacket tp;
  tp.AddChunk({ser_buf.data(), ser_buf.size()});
  auto chunk = tp.begin();
  ASSERT_NE(tp.end(), chunk);
  ASSERT_EQ(ser_buf.data(), chunk->start);
  ASSERT_EQ(ser_buf.size(), chunk->size);
  ASSERT_EQ(tp.end(), ++chunk);

  ASSERT_TRUE(tp.Decode());
  ASSERT_TRUE(tp.Decode());  // Decode() should be idempotent.
  ASSERT_NE(nullptr, tp.operator->());
  ASSERT_EQ(proto.test(), tp->test());
  ASSERT_EQ(proto.test(), (*tp).test());

  // Check move operators.
  TracePacket moved_tp(std::move(tp));
  ASSERT_NE(nullptr, moved_tp.operator->());
  ASSERT_EQ(proto.test(), moved_tp->test());

  TracePacket moved_tp_2;
  moved_tp_2 = std::move(moved_tp);
  ASSERT_NE(nullptr, moved_tp_2.operator->());
  ASSERT_EQ(proto.test(), moved_tp_2->test());
}

TEST(TracePacketTest, Chunked) {
  protos::TracePacket proto;
  proto.set_test("this is an arbitrarily long string ........................");
  std::string ser_buf = proto.SerializeAsString();
  TracePacket tp;
  tp.AddChunk({ser_buf.data(), 3});
  tp.AddChunk({ser_buf.data() + 3, 5});
  tp.AddChunk({ser_buf.data() + 3 + 5, ser_buf.size() - 3 - 5});

  auto chunk = tp.begin();
  ASSERT_NE(tp.end(), chunk);
  ASSERT_EQ(ser_buf.data(), chunk->start);
  ASSERT_EQ(3u, chunk->size);

  ASSERT_NE(tp.end(), ++chunk);
  ASSERT_EQ(ser_buf.data() + 3, chunk->start);
  ASSERT_EQ(5u, chunk->size);

  ASSERT_NE(tp.end(), ++chunk);
  ASSERT_EQ(ser_buf.data() + 3 + 5, chunk->start);
  ASSERT_EQ(ser_buf.size() - 3 - 5, chunk->size);

  ASSERT_EQ(tp.end(), ++chunk);

  ASSERT_TRUE(tp.Decode());
  ASSERT_NE(nullptr, tp.operator->());
  ASSERT_EQ(proto.test(), tp->test());
}

TEST(TracePacketTest, Corrupted) {
  protos::TracePacket proto;
  proto.set_test("string field");
  std::string ser_buf = proto.SerializeAsString();
  TracePacket tp;
  tp.AddChunk({ser_buf.data(), ser_buf.size() - 2});  // corrupted.
  ASSERT_FALSE(tp.Decode());
}

}  // namespace
}  // namespace perfetto
