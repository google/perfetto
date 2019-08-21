/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/perfetto_cmd/packet_writer.h"

#include <string.h>
#include <unistd.h>

#include <random>

#include <zlib.h>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace/test_event.pbzero.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "src/perfetto_cmd/packet_writer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using TracePacketZero = protos::pbzero::TracePacket;

template <typename F>
TracePacket CreateTracePacket(F fill_function) {
  protozero::HeapBuffered<TracePacketZero> msg;
  fill_function(msg.get());
  std::vector<uint8_t> buf = msg.SerializeAsArray();
  Slice slice = Slice::Allocate(buf.size());
  memcpy(slice.own_data(), buf.data(), buf.size());
  perfetto::TracePacket packet;
  packet.AddSlice(std::move(slice));
  return packet;
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

TEST(PacketWriter, FilePacketWriter) {
  base::TempFile tmp = base::TempFile::Create();
  FILE* f = fdopen(tmp.fd(), "wb");

  {
    std::unique_ptr<PacketWriter> writer = CreateFilePacketWriter(f);

    std::vector<perfetto::TracePacket> packets;

    packets.push_back(CreateTracePacket([](TracePacketZero* msg) {
      auto* for_testing = msg->set_for_testing();
      for_testing->set_str("abc");
    }));

    EXPECT_TRUE(writer->WritePackets(std::move(packets)));
  }

  fseek(f, 0, SEEK_SET);
  std::string s;
  EXPECT_TRUE(base::ReadFileStream(f, &s));
  EXPECT_GT(s.size(), 0u);

  protos::Trace trace;
  EXPECT_TRUE(trace.ParseFromString(s));
  EXPECT_EQ(trace.packet().Get(0).for_testing().str(), "abc");
}

TEST(PacketWriter, ZipPacketWriter) {
  base::TempFile tmp = base::TempFile::Create();
  FILE* f = fdopen(tmp.fd(), "wb");

  {
    std::unique_ptr<PacketWriter> writer =
        CreateZipPacketWriter(CreateFilePacketWriter(f));

    std::vector<perfetto::TracePacket> packets;

    packets.push_back(CreateTracePacket([](TracePacketZero* msg) {
      auto* for_testing = msg->set_for_testing();
      for_testing->set_str("abc");
    }));

    EXPECT_TRUE(writer->WritePackets(std::move(packets)));
  }

  std::string s;
  fseek(f, 0, SEEK_SET);
  EXPECT_TRUE(base::ReadFileStream(f, &s));
  EXPECT_GT(s.size(), 0u);

  protos::Trace trace;
  EXPECT_TRUE(trace.ParseFromString(s));

  const std::string& data = trace.packet().Get(0).compressed_packets();
  EXPECT_GT(data.size(), 0u);

  protos::Trace subtrace;
  EXPECT_TRUE(subtrace.ParseFromString(Decompress(data)));
  EXPECT_EQ(subtrace.packet().Get(0).for_testing().str(), "abc");
}

TEST(PacketWriter, ZipPacketWriter_Empty) {
  base::TempFile tmp = base::TempFile::Create();
  FILE* f = fdopen(tmp.fd(), "wb");

  {
    std::unique_ptr<PacketWriter> writer =
        CreateZipPacketWriter(CreateFilePacketWriter(f));
  }

  EXPECT_EQ(fseek(f, 0, SEEK_END), 0);
}

TEST(PacketWriter, ZipPacketWriter_EmptyWithEmptyWrite) {
  base::TempFile tmp = base::TempFile::Create();
  FILE* f = fdopen(tmp.fd(), "wb");

  {
    std::unique_ptr<PacketWriter> writer =
        CreateZipPacketWriter(CreateFilePacketWriter(f));
    writer->WritePackets(std::vector<TracePacket>());
    writer->WritePackets(std::vector<TracePacket>());
    writer->WritePackets(std::vector<TracePacket>());
  }

  EXPECT_EQ(fseek(f, 0, SEEK_END), 0);
}

TEST(PacketWriter, ZipPacketWriter_ShouldCompress) {
  base::TempFile tmp = base::TempFile::Create();
  FILE* f = fdopen(tmp.fd(), "wb");
  size_t uncompressed_size = 0;

  {
    std::unique_ptr<PacketWriter> writer =
        CreateZipPacketWriter(CreateFilePacketWriter(f));

    for (size_t i = 0; i < 200; i++) {
      std::vector<perfetto::TracePacket> packets;

      packets.push_back(CreateTracePacket([](TracePacketZero* msg) {
        auto* for_testing = msg->set_for_testing();
        for_testing->set_str("abcdefghijklmn");
      }));

      packets.push_back(CreateTracePacket([](TracePacketZero* msg) {
        auto* for_testing = msg->set_for_testing();
        for_testing->set_str("abcdefghijklmn");
      }));

      for (const TracePacket& packet : packets)
        uncompressed_size += packet.size();

      EXPECT_TRUE(writer->WritePackets(std::move(packets)));
    }
  }

  std::string s;
  EXPECT_LT(fseek(f, 0, SEEK_END), static_cast<int>(uncompressed_size));
  fseek(f, 0, SEEK_SET);
  EXPECT_TRUE(base::ReadFileStream(f, &s));
  EXPECT_GT(s.size(), 0u);

  protos::Trace trace;
  EXPECT_TRUE(trace.ParseFromString(s));

  size_t packet_count = 0;
  for (const auto& packet : trace.packet()) {
    const std::string& data = packet.compressed_packets();
    EXPECT_GT(data.size(), 0u);
    EXPECT_LT(data.size(), 500 * 1024u);
    protos::Trace subtrace;
    EXPECT_TRUE(subtrace.ParseFromString(Decompress(data)));
    for (const auto& subpacket : subtrace.packet()) {
      packet_count++;
      EXPECT_EQ(subpacket.for_testing().str(), "abcdefghijklmn");
    }
  }

  EXPECT_EQ(packet_count, 200 * 2u);
}

TEST(PacketWriter, ZipPacketWriter_ShouldSplitPackets) {
  base::TempFile tmp = base::TempFile::Create();
  FILE* f = fdopen(tmp.fd(), "wb");

  std::minstd_rand0 rnd(0);
  std::uniform_int_distribution<> dist(0, 255);
  auto randomString = [&dist, &rnd]() {
    std::string s;
    s.resize(1024);
    for (size_t i = 0; i < s.size(); i++)
      s[i] = static_cast<char>(dist(rnd));
    return s;
  };

  {
    std::unique_ptr<PacketWriter> writer =
        CreateZipPacketWriter(CreateFilePacketWriter(f));

    for (uint32_t i = 0; i < 1000; i++) {
      std::vector<perfetto::TracePacket> packets;

      std::string s = randomString();

      packets.push_back(CreateTracePacket([i, &s](TracePacketZero* msg) {
        auto* for_testing = msg->set_for_testing();
        for_testing->set_seq_value(i);
        for_testing->set_str(s.data(), s.size());
      }));

      EXPECT_TRUE(writer->WritePackets(std::move(packets)));
    }
  }

  std::string s;
  fseek(f, 0, SEEK_SET);
  EXPECT_TRUE(base::ReadFileStream(f, &s));
  EXPECT_GT(s.size(), 0u);

  protos::Trace trace;
  EXPECT_TRUE(trace.ParseFromString(s));

  size_t packet_count = 0;
  for (const auto& packet : trace.packet()) {
    const std::string& data = packet.compressed_packets();
    EXPECT_GT(data.size(), 0u);
    EXPECT_LT(data.size(), 500 * 1024u);
    protos::Trace subtrace;
    EXPECT_TRUE(subtrace.ParseFromString(Decompress(data)));
    for (const auto& subpacket : subtrace.packet()) {
      EXPECT_EQ(subpacket.for_testing().seq_value(), packet_count++);
    }
  }

  EXPECT_EQ(packet_count, 1000u);
}

}  // namespace
}  // namespace perfetto
