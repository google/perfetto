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
using StreamProto = protos::pbzero::TraceProcessorRpcStream;
using ResultProto = protos::pbzero::QueryResult;
using BatchProto = protos::pbzero::QueryResult::CellsBatch;

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

std::vector<uint8_t> EncodeQuery(int64_t seq, const std::string& sql) {
  protozero::HeapBuffered<StreamProto> buf;
  auto* msg = buf->add_msg();
  msg->set_seq(seq);
  msg->set_request(RpcProto::TPM_QUERY_STREAMING);
  msg->set_query_args()->set_sql_query(sql);
  return buf.SerializeAsArray();
}

// Collects all varint cells across every QueryResult batch in the response
// stream (for the single-integer queries used here, one value per query).
std::vector<int64_t> DecodeVarints(const std::vector<uint8_t>& buf) {
  std::vector<int64_t> out;
  StreamProto::Decoder stream(buf.data(), buf.size());
  for (auto m = stream.msg(); m; ++m) {
    RpcProto::Decoder rpc(m->as_bytes());
    if (!rpc.has_query_result())
      continue;
    protozero::ConstBytes qr_bytes = rpc.query_result();
    ResultProto::Decoder qr(qr_bytes.data, qr_bytes.size);
    for (auto b = qr.batch(); b; ++b) {
      BatchProto::Decoder batch(b->as_bytes());
      bool parse_error = false;
      for (auto v = batch.varint_cells(&parse_error); v; ++v)
        out.push_back(*v);
    }
  }
  return out;
}

// The response function runs on the (single) thread that calls EndRequest, so
// the ResponseSink needs no synchronization.
TEST(RpcThreadTest, QueryRunsThroughTpThread) {
  Rpc rpc;
  ASSERT_TRUE(rpc.NotifyEndOfFile().ok());

  ResponseSink sink;
  Rpc::Stream stream(rpc, sink.Fn());
  Write(stream, EncodeQuery(/*seq=*/1, "SELECT 42"));

  auto vals = DecodeVarints(sink.bytes);
  ASSERT_EQ(vals.size(), 1u);
  EXPECT_EQ(vals[0], 42);
}

TEST(RpcThreadTest, MultipleSequentialQueries) {
  Rpc rpc;
  ASSERT_TRUE(rpc.NotifyEndOfFile().ok());

  ResponseSink sink;
  Rpc::Stream stream(rpc, sink.Fn());
  for (int i = 0; i < 5; ++i) {
    sink.bytes.clear();
    Write(stream,
          EncodeQuery(/*seq=*/i + 1, "SELECT " + std::to_string(i * 10)));

    auto vals = DecodeVarints(sink.bytes);
    ASSERT_EQ(vals.size(), 1u) << "i=" << i;
    EXPECT_EQ(vals[0], i * 10);
  }
}

// A query that returns many rows forces the serializer to emit multiple
// batches, exercising the per-batch emit ping-pong between the TP and receiving
// threads.
TEST(RpcThreadTest, MultiBatchQueryStreamsAllRows) {
  Rpc rpc;
  ASSERT_TRUE(rpc.NotifyEndOfFile().ok());

  ResponseSink sink;
  Rpc::Stream stream(rpc, sink.Fn());
  Write(stream,
        EncodeQuery(
            /*seq=*/1,
            "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c "
            "WHERE x < 5000) SELECT x FROM c"));

  auto vals = DecodeVarints(sink.bytes);
  ASSERT_EQ(vals.size(), 5000u);
  EXPECT_EQ(vals.front(), 1);
  EXPECT_EQ(vals.back(), 5000);
}

// Two messages in one buffer become two sequential tasks on the TP thread; the
// finalize must complete before the query runs.
TEST(RpcThreadTest, BytePipeFinalizeThenQuery) {
  Rpc rpc;
  ResponseSink sink;
  Rpc::Stream stream(rpc, sink.Fn());

  std::vector<uint8_t> buf;
  auto fin = RpcMessage(/*seq=*/1, RpcProto::TPM_FINALIZE_TRACE_DATA);
  auto q = EncodeQuery(/*seq=*/2, "SELECT 7");
  buf.insert(buf.end(), fin.begin(), fin.end());
  buf.insert(buf.end(), q.begin(), q.end());
  Write(stream, buf);

  auto vals = DecodeVarints(sink.bytes);
  ASSERT_EQ(vals.size(), 1u);
  EXPECT_EQ(vals[0], 7);
}

// The direct (non-byte-pipe) legacy methods go through the public wrappers,
// which marshal to the TP thread too.
TEST(RpcThreadTest, DirectLegacyMethodsRunOnTpThread) {
  Rpc rpc;
  ASSERT_TRUE(rpc.NotifyEndOfFile().ok());
  std::vector<uint8_t> status = rpc.GetStatus();
  EXPECT_FALSE(status.empty());
}

}  // namespace
}  // namespace perfetto::trace_processor
