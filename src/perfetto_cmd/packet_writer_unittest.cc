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

#include <random>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "src/perfetto_cmd/packet_writer.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto {
namespace {

using TracePacketProto = protos::gen::TracePacket;

template <typename F>
TracePacket CreateTracePacket(F fill_function) {
  TracePacketProto msg;
  fill_function(&msg);
  std::vector<uint8_t> buf = msg.SerializeAsArray();
  Slice slice = Slice::Allocate(buf.size());
  memcpy(slice.own_data(), buf.data(), buf.size());
  perfetto::TracePacket packet;
  packet.AddSlice(std::move(slice));
  return packet;
}

TEST(PacketWriterTest, FilePacketWriter) {
  base::TempFile tmp = base::TempFile::CreateUnlinked();
  base::ScopedResource<FILE*, fclose, nullptr> f(
      fdopen(tmp.ReleaseFD().release(), "wb"));

  std::vector<perfetto::TracePacket> packets;
  packets.push_back(CreateTracePacket([](TracePacketProto* msg) {
    auto* for_testing = msg->mutable_for_testing();
    for_testing->set_str("abc");
  }));

  {
    PacketWriter writer(*f);
    EXPECT_TRUE(writer.WritePackets(std::move(packets)));
  }

  fseek(*f, 0, SEEK_SET);
  std::string s;
  EXPECT_TRUE(base::ReadFileStream(*f, &s));
  EXPECT_GT(s.size(), 0u);

  protos::gen::Trace trace;
  EXPECT_TRUE(trace.ParseFromString(s));
  EXPECT_EQ(trace.packet()[0].for_testing().str(), "abc");
}

}  // namespace
}  // namespace perfetto
