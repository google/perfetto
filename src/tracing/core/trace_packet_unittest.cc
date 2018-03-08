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

#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {
namespace {

TEST(TracePacketTest, Simple) {
  protos::TracePacket proto;
  proto.mutable_for_testing()->set_str("string field");
  std::string ser_buf = proto.SerializeAsString();
  TracePacket tp;
  tp.AddSlice(ser_buf.data(), ser_buf.size());
  auto slice = tp.slices().begin();
  ASSERT_NE(tp.slices().end(), slice);
  ASSERT_EQ(ser_buf.data(), slice->start);
  ASSERT_EQ(ser_buf.size(), slice->size);
  ASSERT_EQ(tp.slices().end(), ++slice);

  ASSERT_TRUE(tp.Decode());
  ASSERT_TRUE(tp.Decode());  // Decode() should be idempotent.
  ASSERT_NE(nullptr, tp.operator->());
  ASSERT_EQ(proto.for_testing().str(), tp->for_testing().str());
  ASSERT_EQ(proto.for_testing().str(), (*tp).for_testing().str());

  // Check move operators.
  TracePacket moved_tp(std::move(tp));
  ASSERT_NE(nullptr, moved_tp.operator->());
  ASSERT_EQ(proto.for_testing().str(), moved_tp->for_testing().str());

  TracePacket moved_tp_2;
  moved_tp_2 = std::move(moved_tp);
  ASSERT_NE(nullptr, moved_tp_2.operator->());
  ASSERT_EQ(proto.for_testing().str(), moved_tp_2->for_testing().str());
}

TEST(TracePacketTest, Sliced) {
  protos::TracePacket proto;
  proto.mutable_for_testing()->set_str(
      "this is an arbitrarily long string ........................");
  std::string ser_buf = proto.SerializeAsString();
  TracePacket tp;
  tp.AddSlice({ser_buf.data(), 3});
  tp.AddSlice({ser_buf.data() + 3, 5});
  tp.AddSlice({ser_buf.data() + 3 + 5, ser_buf.size() - 3 - 5});
  ASSERT_EQ(ser_buf.size(), tp.size());

  auto slice = tp.slices().begin();
  ASSERT_NE(tp.slices().end(), slice);
  ASSERT_EQ(ser_buf.data(), slice->start);
  ASSERT_EQ(3u, slice->size);

  ASSERT_NE(tp.slices().end(), ++slice);
  ASSERT_EQ(ser_buf.data() + 3, slice->start);
  ASSERT_EQ(5u, slice->size);

  ASSERT_NE(tp.slices().end(), ++slice);
  ASSERT_EQ(ser_buf.data() + 3 + 5, slice->start);
  ASSERT_EQ(ser_buf.size() - 3 - 5, slice->size);

  ASSERT_EQ(tp.slices().end(), ++slice);

  ASSERT_TRUE(tp.Decode());
  ASSERT_NE(nullptr, tp.operator->());
  ASSERT_EQ(proto.for_testing().str(), tp->for_testing().str());
}

TEST(TracePacketTest, Corrupted) {
  protos::TracePacket proto;
  proto.mutable_for_testing()->set_str("string field");
  std::string ser_buf = proto.SerializeAsString();
  TracePacket tp;
  tp.AddSlice({ser_buf.data(), ser_buf.size() - 2});  // corrupted.
  ASSERT_FALSE(tp.Decode());
}

}  // namespace
}  // namespace perfetto
