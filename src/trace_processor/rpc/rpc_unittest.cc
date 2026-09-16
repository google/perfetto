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

#include "src/trace_processor/rpc/rpc.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto::trace_processor {
namespace {

using ::testing::HasSubstr;
using RpcProto = protos::pbzero::TraceProcessorRpc;

// Collects everything a Stream sends back, and whether it was disconnected.
class ResponseSink {
 public:
  Rpc::RpcResponseFunction Fn() {
    return [this](const void* data, uint32_t len) {
      if (data == nullptr) {
        disconnected = true;
        return;
      }
      auto* p = static_cast<const uint8_t*>(data);
      bytes.insert(bytes.end(), p, p + len);
    };
  }

  // The responses concatenated, for substring assertions on error strings.
  std::string AsString() const {
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
  }

  std::vector<uint8_t> bytes;
  bool disconnected = false;
};

// A TraceProcessorRpc{seq, request} framed as a TraceProcessorRpcStream.msg.
std::vector<uint8_t> RpcMessage(int64_t seq, RpcProto::TraceProcessorMethod m) {
  protozero::HeapBuffered<protos::pbzero::TraceProcessorRpcStream> stream;
  auto* msg = stream->add_msg();
  msg->set_seq(seq);
  msg->set_request(m);
  return stream.SerializeAsArray();
}

void Write(Rpc::Stream& stream, const std::vector<uint8_t>& data) {
  auto write = stream.BeginRequest(data.size());
  memcpy(write.data(), data.data(), data.size());
  write.EndRequest(data.size());
}

// The sequence ids stay shared across streams on purpose: a second peer
// numbering from its own 0 is how two clients on one TraceProcessor are
// detected. Streams must not paper over that.
TEST(RpcStreamTest, SecondPeerStillTripsTheSequenceCheck) {
  Rpc rpc;
  ResponseSink sink_a;
  ResponseSink sink_b;
  Rpc::Stream a(rpc, sink_a.Fn());
  Rpc::Stream b(rpc, sink_b.Fn());

  Write(a, RpcMessage(1, RpcProto::TPM_GET_STATUS));
  Write(a, RpcMessage(2, RpcProto::TPM_GET_STATUS));
  EXPECT_FALSE(sink_a.disconnected);
  EXPECT_THAT(sink_a.AsString(), ::testing::Not(HasSubstr("ERR:rpc_seq")));

  // |b| restarts from 1, which cannot follow |a|'s 2.
  Write(b, RpcMessage(1, RpcProto::TPM_GET_STATUS));
  EXPECT_THAT(sink_b.AsString(), HasSubstr("ERR:rpc_seq"));
  EXPECT_TRUE(sink_b.disconnected);
}

// A single peer on its own stream is undisturbed by the check.
TEST(RpcStreamTest, OnePeerRunsItsSequenceToCompletion) {
  Rpc rpc;
  ResponseSink sink;
  Rpc::Stream stream(rpc, sink.Fn());

  for (int64_t seq = 1; seq <= 3; seq++)
    Write(stream, RpcMessage(seq, RpcProto::TPM_GET_STATUS));

  EXPECT_FALSE(sink.disconnected);
  EXPECT_THAT(sink.AsString(), ::testing::Not(HasSubstr("ERR:rpc_seq")));
  EXPECT_FALSE(sink.bytes.empty());
}

// A message split across reads must not be corrupted by another stream's
// bytes landing in between: the half-received message lives in the stream.
TEST(RpcStreamTest, InterleavedPartialMessagesDoNotCorruptEachOther) {
  Rpc rpc;
  ResponseSink sink_a;
  ResponseSink sink_b;
  Rpc::Stream a(rpc, sink_a.Fn());
  Rpc::Stream b(rpc, sink_b.Fn());

  // Consecutive seq ids: the point here is framing, and the shared sequence
  // check would otherwise fire first and mask it.
  auto msg_a = RpcMessage(1, RpcProto::TPM_GET_STATUS);
  auto msg_b = RpcMessage(2, RpcProto::TPM_GET_STATUS);

  // Split both in half and interleave the halves.
  std::vector<uint8_t> a1(msg_a.begin(), msg_a.begin() + 3);
  std::vector<uint8_t> a2(msg_a.begin() + 3, msg_a.end());
  std::vector<uint8_t> b1(msg_b.begin(), msg_b.begin() + 3);
  std::vector<uint8_t> b2(msg_b.begin() + 3, msg_b.end());

  Write(a, a1);
  Write(b, b1);
  // Neither is complete yet, so nothing should have been dispatched.
  EXPECT_TRUE(sink_a.bytes.empty());
  EXPECT_TRUE(sink_b.bytes.empty());

  Write(a, a2);
  Write(b, b2);

  EXPECT_FALSE(sink_a.disconnected);
  EXPECT_FALSE(sink_b.disconnected);
  EXPECT_FALSE(sink_a.bytes.empty());
  EXPECT_FALSE(sink_b.bytes.empty());
  EXPECT_THAT(sink_a.AsString(), ::testing::Not(HasSubstr("framing error")));
  EXPECT_THAT(sink_b.AsString(), ::testing::Not(HasSubstr("framing error")));
  EXPECT_THAT(sink_b.AsString(), ::testing::Not(HasSubstr("ERR:rpc_seq")));
}

// A stream that dies mid-message takes its own framing state with it.
TEST(RpcStreamTest, ClosingStreamMidMessageLeavesOthersAlone) {
  Rpc rpc;
  ResponseSink sink_b;
  Rpc::Stream b(rpc, sink_b.Fn());

  {
    ResponseSink sink_a;
    Rpc::Stream a(rpc, sink_a.Fn());
    auto msg = RpcMessage(1, RpcProto::TPM_GET_STATUS);
    Write(a, {msg.begin(), msg.begin() + 3});  // Half a message, then gone.
  }

  Write(b, RpcMessage(1, RpcProto::TPM_GET_STATUS));
  EXPECT_FALSE(sink_b.disconnected);
  EXPECT_FALSE(sink_b.bytes.empty());
  EXPECT_THAT(sink_b.AsString(), ::testing::Not(HasSubstr("framing error")));
}

// A framing error is reported to the stream that caused it, and only it.
TEST(RpcStreamTest, FramingErrorIsScopedToOneStream) {
  Rpc rpc;
  ResponseSink sink_a;
  ResponseSink sink_b;
  Rpc::Stream a(rpc, sink_a.Fn());
  Rpc::Stream b(rpc, sink_b.Fn());

  // Field 1 with a non-length-delimited wire type: an unrecoverable framing
  // error.
  const std::vector<uint8_t> garbage{0x08, 0x00};
  Write(a, garbage);

  EXPECT_TRUE(sink_a.disconnected);
  EXPECT_THAT(sink_a.AsString(), HasSubstr("RPC framing error"));

  Write(b, RpcMessage(1, RpcProto::TPM_GET_STATUS));
  EXPECT_FALSE(sink_b.disconnected);
  EXPECT_THAT(sink_b.AsString(), ::testing::Not(HasSubstr("framing error")));
}

}  // namespace
}  // namespace perfetto::trace_processor
