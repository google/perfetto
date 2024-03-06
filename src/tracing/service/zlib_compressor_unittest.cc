/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/tracing/service/zlib_compressor.h"

#include <random>

#include <zlib.h>

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "src/tracing/service/tracing_service_impl.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::IsEmpty;
using ::testing::Le;
using ::testing::Not;
using ::testing::Property;
using ::testing::SizeIs;

template <typename F>
TracePacket CreateTracePacket(F fill_function) {
  protos::gen::TracePacket msg;
  fill_function(&msg);
  std::vector<uint8_t> buf = msg.SerializeAsArray();
  Slice slice = Slice::Allocate(buf.size());
  memcpy(slice.own_data(), buf.data(), buf.size());
  perfetto::TracePacket packet;
  packet.AddSlice(std::move(slice));
  return packet;
}

// Return a copy of the `old` trace packets that owns its own slices data.
TracePacket CopyTracePacket(const TracePacket& old) {
  TracePacket ret;
  for (const Slice& slice : old.slices()) {
    auto new_slice = Slice::Allocate(slice.size);
    memcpy(new_slice.own_data(), slice.start, slice.size);
    ret.AddSlice(std::move(new_slice));
  }
  return ret;
}

std::vector<TracePacket> CopyTracePackets(const std::vector<TracePacket>& old) {
  std::vector<TracePacket> ret;
  ret.reserve(old.size());
  for (const TracePacket& trace_packet : old) {
    ret.push_back(CopyTracePacket(trace_packet));
  }
  return ret;
}
std::string RandomString(size_t size) {
  std::default_random_engine rnd(0);
  std::uniform_int_distribution<> dist(0, 255);
  std::string s;
  s.resize(size);
  for (size_t i = 0; i < s.size(); i++)
    s[i] = static_cast<char>(dist(rnd));
  return s;
}

std::string Decompress(const std::string& data) {
  uint8_t out[1024];

  z_stream stream{};
  stream.next_in = reinterpret_cast<uint8_t*>(const_cast<char*>(data.data()));
  stream.avail_in = static_cast<unsigned int>(data.size());

  EXPECT_EQ(inflateInit(&stream), Z_OK);
  std::string s;

  int ret;
  do {
    stream.next_out = out;
    stream.avail_out = sizeof(out);
    ret = inflate(&stream, Z_NO_FLUSH);
    EXPECT_NE(ret, Z_STREAM_ERROR);
    EXPECT_NE(ret, Z_NEED_DICT);
    EXPECT_NE(ret, Z_DATA_ERROR);
    EXPECT_NE(ret, Z_MEM_ERROR);
    s.append(reinterpret_cast<char*>(out), sizeof(out) - stream.avail_out);
  } while (ret != Z_STREAM_END);

  inflateEnd(&stream);
  return s;
}

static_assert(kZlibCompressSliceSize ==
              TracingServiceImpl::kMaxTracePacketSliceSize);

TEST(ZlibCompressFnTest, Empty) {
  std::vector<TracePacket> packets;

  ZlibCompressFn(&packets);

  EXPECT_THAT(packets, IsEmpty());
}

TEST(ZlibCompressFnTest, End2EndCompressAndDecompress) {
  std::vector<TracePacket> packets;

  packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
    auto* for_testing = msg->mutable_for_testing();
    for_testing->set_str("abc");
  }));
  packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
    auto* for_testing = msg->mutable_for_testing();
    for_testing->set_str("def");
  }));

  ZlibCompressFn(&packets);

  ASSERT_THAT(packets, SizeIs(1));
  protos::gen::TracePacket compressed_packet_proto;
  ASSERT_TRUE(compressed_packet_proto.ParseFromString(
      packets[0].GetRawBytesForTesting()));
  const std::string& data = compressed_packet_proto.compressed_packets();
  EXPECT_THAT(data, Not(IsEmpty()));
  protos::gen::Trace subtrace;
  ASSERT_TRUE(subtrace.ParseFromString(Decompress(data)));
  EXPECT_THAT(
      subtrace.packet(),
      ElementsAre(Property(&protos::gen::TracePacket::for_testing,
                           Property(&protos::gen::TestEvent::str, "abc")),
                  Property(&protos::gen::TracePacket::for_testing,
                           Property(&protos::gen::TestEvent::str, "def"))));
}

TEST(ZlibCompressFnTest, MaxSliceSize) {
  std::vector<TracePacket> packets;

  constexpr size_t kStopOutputSize =
      TracingServiceImpl::kMaxTracePacketSliceSize + 2000;

  TracePacket compressed_packet;
  while (compressed_packet.size() < kStopOutputSize) {
    packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
      auto* for_testing = msg->mutable_for_testing();
      for_testing->set_str(RandomString(65536));
    }));
    {
      std::vector<TracePacket> packets_copy = CopyTracePackets(packets);
      ZlibCompressFn(&packets_copy);
      ASSERT_THAT(packets_copy, SizeIs(1));
      compressed_packet = std::move(packets_copy[0]);
    }
  }

  EXPECT_GE(compressed_packet.slices().size(), 2u);
  ASSERT_GT(compressed_packet.size(),
            TracingServiceImpl::kMaxTracePacketSliceSize);
  EXPECT_THAT(compressed_packet.slices(),
              Each(Field(&Slice::size,
                         Le(TracingServiceImpl::kMaxTracePacketSliceSize))));
}

}  // namespace
}  // namespace perfetto
