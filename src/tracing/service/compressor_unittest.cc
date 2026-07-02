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

#include "perfetto/base/build_config.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "perfetto/tracing/core/trace_config.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "src/tracing/service/packet_compressor_common.h"
#include "src/tracing/service/tracing_service_impl.h"
#include "test/gtest_and_gmock.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
#include <zlib.h>
#include "src/tracing/service/zlib_compressor.h"
#endif
#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
#include <zstd.h>
#include "src/tracing/service/zstd_compressor.h"
#endif

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
using tracing_service::TracingServiceImpl;

// The compressors cap their output slices at the service's max slice size.
static_assert(packet_compressor::kCompressSliceSize ==
              TracingServiceImpl::kMaxTracePacketSliceSize);

// One backend per compile-time-selected compressor. Each provides the compress
// entrypoint and a matching decompressor for the test to verify the output.
#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
struct ZlibBackend {
  static constexpr const char* kName = "Zlib";
  static void Compress(std::vector<TracePacket>* packets, int /*level*/ = 0) {
    ZlibCompressFn(packets);
  }
  static std::string Decompress(const std::string& data) {
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
};
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZLIB)

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
struct ZstdBackend {
  static constexpr const char* kName = "Zstd";
  static void Compress(std::vector<TracePacket>* packets, int level = 0) {
    TraceConfig::CompressionConfig::Zstd zstd;
    zstd.set_level(level);
    ZstdCompressFn(packets, zstd);
  }
  static std::string Decompress(const std::string& data) {
    ZSTD_DStream* stream = ZSTD_createDStream();
    EXPECT_NE(stream, nullptr);
    ZSTD_initDStream(stream);
    uint8_t out[1024];
    ZSTD_inBuffer in = {data.data(), data.size(), 0};
    std::string s;
    size_t ret = 0;
    do {
      ZSTD_outBuffer out_buf = {out, sizeof(out), 0};
      ret = ZSTD_decompressStream(stream, &out_buf, &in);
      EXPECT_FALSE(ZSTD_isError(ret));
      s.append(reinterpret_cast<char*>(out), out_buf.pos);
    } while (ret != 0 && in.pos < in.size);
    ZSTD_freeDStream(stream);
    return s;
  }
};
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

using Backends = ::testing::Types<
#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
    ZlibBackend
#endif
#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB) && PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
    ,
#endif
#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
    ZstdBackend
#endif
    >;

class BackendNames {
 public:
  template <typename T>
  static std::string GetName(int) {
    return T::kName;
  }
};

template <typename Backend>
class CompressorTest : public ::testing::Test {};
TYPED_TEST_SUITE(CompressorTest, Backends, BackendNames);

template <typename F>
TracePacket CreateTracePacket(F fill_function) {
  protos::gen::TracePacket msg;
  fill_function(&msg);
  std::vector<uint8_t> buf = msg.SerializeAsArray();
  Slice slice = Slice::Allocate(buf.size());
  memcpy(slice.own_data(), buf.data(), buf.size());
  TracePacket packet;
  packet.AddSlice(std::move(slice));
  return packet;
}

// Returns a copy of `old` that owns its slices' data.
TracePacket CopyTracePacket(const TracePacket& old) {
  TracePacket ret;
  for (const Slice& slice : old.slices()) {
    Slice new_slice = Slice::Allocate(slice.size);
    memcpy(new_slice.own_data(), slice.start, slice.size);
    ret.AddSlice(std::move(new_slice));
  }
  return ret;
}

std::vector<TracePacket> CopyTracePackets(const std::vector<TracePacket>& old) {
  std::vector<TracePacket> ret;
  ret.reserve(old.size());
  for (const TracePacket& packet : old)
    ret.push_back(CopyTracePacket(packet));
  return ret;
}

// Distinct random bytes on every call: incompressible, and crucially never
// repeating across packets, so zstd (which dedupes across its window) can't
// collapse the MaxSliceSize input to nothing.
std::string RandomString(size_t size) {
  static uint32_t seed = 0;
  std::default_random_engine rnd(seed++);
  std::uniform_int_distribution<> dist(0, 255);
  std::string s(size, '\0');
  for (char& c : s)
    c = static_cast<char>(dist(rnd));
  return s;
}

TYPED_TEST(CompressorTest, Empty) {
  std::vector<TracePacket> packets;

  TypeParam::Compress(&packets);

  EXPECT_THAT(packets, IsEmpty());
}

TYPED_TEST(CompressorTest, End2EndCompressAndDecompress) {
  std::vector<TracePacket> packets;
  packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
    msg->mutable_for_testing()->set_str("abc");
  }));
  packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
    msg->mutable_for_testing()->set_str("def");
  }));

  TypeParam::Compress(&packets);

  ASSERT_THAT(packets, SizeIs(1));
  protos::gen::TracePacket compressed_packet_proto;
  ASSERT_TRUE(compressed_packet_proto.ParseFromString(
      packets[0].GetRawBytesForTesting()));
  const std::string& data = compressed_packet_proto.compressed_packets();
  EXPECT_THAT(data, Not(IsEmpty()));
  protos::gen::Trace subtrace;
  ASSERT_TRUE(subtrace.ParseFromString(TypeParam::Decompress(data)));
  EXPECT_THAT(
      subtrace.packet(),
      ElementsAre(Property(&protos::gen::TracePacket::for_testing,
                           Property(&protos::gen::TestEvent::str, "abc")),
                  Property(&protos::gen::TracePacket::for_testing,
                           Property(&protos::gen::TestEvent::str, "def"))));
}

TYPED_TEST(CompressorTest, MaxSliceSize) {
  constexpr size_t kStopOutputSize =
      TracingServiceImpl::kMaxTracePacketSliceSize + 2000;

  std::vector<TracePacket> packets;
  TracePacket compressed_packet;
  while (compressed_packet.size() < kStopOutputSize) {
    packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
      msg->mutable_for_testing()->set_str(RandomString(65536));
    }));
    std::vector<TracePacket> packets_copy = CopyTracePackets(packets);
    TypeParam::Compress(&packets_copy);
    ASSERT_THAT(packets_copy, SizeIs(1));
    compressed_packet = std::move(packets_copy[0]);
  }

  EXPECT_GE(compressed_packet.slices().size(), 2u);
  ASSERT_GT(compressed_packet.size(),
            TracingServiceImpl::kMaxTracePacketSliceSize);
  EXPECT_THAT(compressed_packet.slices(),
              Each(Field(&Slice::size,
                         Le(TracingServiceImpl::kMaxTracePacketSliceSize))));
}

// Round-trips many distinct packets and checks they come back in order. This
// exercises the per-packet framing/tokenization beyond the two-packet case.
TYPED_TEST(CompressorTest, ManyPacketsPreserveOrder) {
  constexpr size_t kNumPackets = 100;
  std::vector<TracePacket> packets;
  for (size_t i = 0; i < kNumPackets; i++) {
    packets.push_back(CreateTracePacket([i](protos::gen::TracePacket* msg) {
      msg->mutable_for_testing()->set_str("packet-" + std::to_string(i));
    }));
  }

  TypeParam::Compress(&packets);

  ASSERT_THAT(packets, SizeIs(1));
  protos::gen::TracePacket compressed;
  ASSERT_TRUE(compressed.ParseFromString(packets[0].GetRawBytesForTesting()));
  protos::gen::Trace subtrace;
  ASSERT_TRUE(subtrace.ParseFromString(
      TypeParam::Decompress(compressed.compressed_packets())));
  ASSERT_THAT(subtrace.packet(), SizeIs(kNumPackets));
  for (size_t i = 0; i < kNumPackets; i++) {
    EXPECT_EQ(subtrace.packet()[i].for_testing().str(),
              "packet-" + std::to_string(i));
  }
}

// Compresses enough incompressible data that the output spans multiple slices,
// then decompresses and checks the exact content survives. End2End only covers
// single-slice output; MaxSliceSize checks slice sizes but never round-trips.
TYPED_TEST(CompressorTest, MultiSliceOutputRoundTrip) {
  std::vector<std::string> expected;
  std::vector<TracePacket> packets;
  for (int i = 0; i < 4; i++) {
    std::string payload = RandomString(65536);
    expected.push_back(payload);
    packets.push_back(CreateTracePacket([&](protos::gen::TracePacket* msg) {
      msg->mutable_for_testing()->set_str(payload);
    }));
  }

  TypeParam::Compress(&packets);

  ASSERT_THAT(packets, SizeIs(1));
  EXPECT_GE(packets[0].slices().size(), 2u);  // Output spans multiple slices.
  protos::gen::TracePacket compressed;
  ASSERT_TRUE(compressed.ParseFromString(packets[0].GetRawBytesForTesting()));
  protos::gen::Trace subtrace;
  ASSERT_TRUE(subtrace.ParseFromString(
      TypeParam::Decompress(compressed.compressed_packets())));
  ASSERT_THAT(subtrace.packet(), SizeIs(expected.size()));
  for (size_t i = 0; i < expected.size(); i++)
    EXPECT_EQ(subtrace.packet()[i].for_testing().str(), expected[i]);
}

// A TracePacket whose payload is split across several input slices (as the
// service produces) must round-trip. Exercises the multi-slice loop in
// PushPacket, which the other tests (single-slice packets) never hit.
TYPED_TEST(CompressorTest, MultiSliceInputPacket) {
  protos::gen::TracePacket msg;
  msg.mutable_for_testing()->set_str("multi-slice-input-payload");
  std::vector<uint8_t> buf = msg.SerializeAsArray();
  ASSERT_GT(buf.size(), 8u);

  TracePacket packet;
  constexpr size_t kInSliceSize = 4;  // Tiny, to force several input slices.
  for (size_t pos = 0; pos < buf.size(); pos += kInSliceSize) {
    size_t n = std::min(kInSliceSize, buf.size() - pos);
    Slice slice = Slice::Allocate(n);
    memcpy(slice.own_data(), &buf[pos], n);
    packet.AddSlice(std::move(slice));
  }
  ASSERT_GT(packet.slices().size(), 1u);

  std::vector<TracePacket> packets;
  packets.push_back(std::move(packet));

  TypeParam::Compress(&packets);

  ASSERT_THAT(packets, SizeIs(1));
  protos::gen::TracePacket compressed;
  ASSERT_TRUE(compressed.ParseFromString(packets[0].GetRawBytesForTesting()));
  protos::gen::Trace subtrace;
  ASSERT_TRUE(subtrace.ParseFromString(
      TypeParam::Decompress(compressed.compressed_packets())));
  ASSERT_THAT(subtrace.packet(), SizeIs(1));
  EXPECT_EQ(subtrace.packet()[0].for_testing().str(),
            "multi-slice-input-payload");
}

// An explicit level must reach the codec: the output still round-trips, and on
// compressible data a higher level is never worse than a lower one (catching
// the level being silently dropped).
TYPED_TEST(CompressorTest, CompressionLevelIsHonored) {
  auto make_packets = [] {
    std::vector<TracePacket> packets;
    for (int i = 0; i < 200; i++) {
      packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
        msg->mutable_for_testing()->set_str(std::string(1024, 'a'));
      }));
    }
    return packets;
  };

  auto compress_and_measure = [](std::vector<TracePacket> packets,
                                 int level) -> size_t {
    TypeParam::Compress(&packets, level);
    EXPECT_THAT(packets, SizeIs(1));
    protos::gen::TracePacket compressed;
    EXPECT_TRUE(compressed.ParseFromString(packets[0].GetRawBytesForTesting()));
    protos::gen::Trace subtrace;
    EXPECT_TRUE(subtrace.ParseFromString(
        TypeParam::Decompress(compressed.compressed_packets())));
    EXPECT_THAT(subtrace.packet(), SizeIs(200));
    return packets[0].size();
  };

  size_t low_level_size = compress_and_measure(make_packets(), 1);
  size_t high_level_size = compress_and_measure(make_packets(), 9);

  EXPECT_GT(low_level_size, 0u);
  EXPECT_LE(high_level_size, low_level_size);
}

}  // namespace
}  // namespace perfetto
