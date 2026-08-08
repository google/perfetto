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

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "perfetto/ext/protozero/proto_ring_buffer.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"
#include "src/base/test/test_task_runner.h"
#include "test/gtest_and_gmock.h"

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

using RpcStream = protos::pbzero::TraceProcessorRpcStream;

std::vector<uint8_t> QueryRequest(int64_t seq,
                                  const std::string& sql,
                                  const std::string& tag,
                                  bool cancellable) {
  protozero::HeapBuffered<RpcStream> stream;
  auto* request = stream->add_msg();
  request->set_seq(seq);
  request->set_request(RpcProto::TPM_QUERY_STREAMING);
  auto* args = request->set_query_args();
  args->set_sql_query(sql);
  args->set_tag(tag);
  args->set_cancellable(cancellable);
  return stream.SerializeAsArray();
}

std::vector<uint8_t> InterruptRequest(int64_t seq, const std::string& tag) {
  protozero::HeapBuffered<RpcStream> stream;
  auto* request = stream->add_msg();
  request->set_seq(seq);
  request->set_request(RpcProto::TPM_INTERRUPT_QUERY);
  request->set_interrupt_query_args()->set_tag(tag);
  return stream.SerializeAsArray();
}

struct TerminalQueryResponse {
  int64_t seq = 0;
  std::string error;
  int32_t error_code = 0;
};

class ResponseCollector {
 public:
  Rpc::RpcResponseFunction callback() {
    return [this](const void* data, uint32_t len) {
      std::lock_guard<std::mutex> lock(mutex_);
      ASSERT_NE(data, nullptr);
      auto write = rxbuf_.BeginWrite(len);
      memcpy(write.data(), data, len);
      write.EndWrite(len);
      for (;;) {
        auto message = rxbuf_.ReadMessage();
        if (!message.valid())
          break;
        RpcProto::Decoder response(message.data(), message.size());
        if (response.response() != RpcProto::TPM_QUERY_STREAMING ||
            !response.has_query_result()) {
          continue;
        }
        protos::pbzero::QueryResult::Decoder result(response.query_result());
        bool terminal = false;
        for (auto it = result.batch(); it; ++it) {
          protozero::ConstBytes bytes = it->as_bytes();
          protos::pbzero::QueryResult::CellsBatch::Decoder batch(bytes.data,
                                                                 bytes.size);
          terminal |= batch.is_last_batch();
        }
        if (!terminal)
          continue;
        TerminalQueryResponse item;
        item.seq = response.seq();
        if (result.has_error())
          item.error = result.error().ToStdString();
        if (result.has_error_code())
          item.error_code = result.error_code();
        responses_.push_back(std::move(item));
        cv_.notify_all();
      }
    };
  }

  std::vector<TerminalQueryResponse> WaitFor(size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    EXPECT_TRUE(cv_.wait_for(lock, std::chrono::seconds(10),
                             [&] { return responses_.size() >= count; }));
    return responses_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  protozero::ProtoRingBuffer rxbuf_;
  std::vector<TerminalQueryResponse> responses_;
};

TEST(TraceProcessorRpcTest, InterruptIgnoredWhenSeveralRequestsOutstanding) {
  Rpc rpc;
  rpc.EnableThreadedExecution();
  ResponseCollector responses;
  Rpc::Stream stream(rpc, responses.callback());

  // With two requests outstanding the interrupt must leave both alone.
  const std::string slow_query =
      "WITH RECURSIVE x(v) AS (VALUES(0) UNION ALL SELECT v + 1 FROM x "
      "WHERE v < 200000) SELECT sum(v) FROM x";
  auto blocker = QueryRequest(1, slow_query, "blocker", false);
  auto target = QueryRequest(2, "SELECT 2", "target", true);
  auto interrupt = InterruptRequest(3, "target");
  Write(stream, blocker);
  Write(stream, target);
  Write(stream, interrupt);

  auto result = responses.WaitFor(2);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_TRUE(result[0].error.empty());
  EXPECT_TRUE(result[1].error.empty()) << result[1].error;
}

TEST(TraceProcessorRpcTest, InterruptLeavesAnotherTagAlone) {
  Rpc rpc;
  rpc.EnableThreadedExecution();
  ResponseCollector responses;
  Rpc::Stream stream(rpc, responses.callback());

  const std::string slow_query =
      "WITH RECURSIVE x(v) AS (VALUES(0) UNION ALL SELECT v + 1 FROM x "
      "WHERE v < 200000) SELECT sum(v) FROM x";
  auto running = QueryRequest(1, slow_query, "mine", true);
  Write(stream, running);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  auto interrupt = InterruptRequest(2, "theirs");
  Write(stream, interrupt);

  auto result = responses.WaitFor(1);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_TRUE(result[0].error.empty()) << result[0].error;
}

TEST(TraceProcessorRpcTest, InterruptsRunningCancellableQuery) {
  Rpc rpc;
  rpc.EnableThreadedExecution();
  ResponseCollector responses;
  Rpc::Stream stream(rpc, responses.callback());

  const std::string slow_query =
      "WITH RECURSIVE x(v) AS (VALUES(0) UNION ALL SELECT v + 1 FROM x "
      "WHERE v < 10000000) SELECT sum(v) FROM x";
  auto running = QueryRequest(1, slow_query, "target", true);
  Write(stream, running);
  // Let the execution thread pick the query up first.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  auto interrupt = InterruptRequest(2, "target");
  Write(stream, interrupt);

  auto result = responses.WaitFor(1);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].error_code,
            protos::pbzero::QueryResult::ERROR_CODE_INTERRUPTED)
      << result[0].error;
  EXPECT_FALSE(result[0].error.empty());
}

TEST(TraceProcessorRpcTest, ResponsesAreDeliveredOnTheStreamsTaskRunner) {
  Rpc rpc;
  rpc.EnableThreadedExecution();
  base::TestTaskRunner task_runner;
  ResponseCollector responses;
  const std::thread::id transport_thread = std::this_thread::get_id();

  Rpc::RpcResponseFunction collect = responses.callback();
  int completed = 0;
  Rpc::Stream stream(
      rpc,
      [&](const void* data, uint32_t len) {
        EXPECT_EQ(std::this_thread::get_id(), transport_thread);
        collect(data, len);
      },
      [&, done = task_runner.CreateCheckpoint("complete")] {
        EXPECT_EQ(std::this_thread::get_id(), transport_thread);
        completed++;
        done();
      },
      &task_runner);

  Write(stream, QueryRequest(1, "SELECT 1", "tag", false));
  task_runner.RunUntilCheckpoint("complete");

  auto result = responses.WaitFor(1);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_TRUE(result[0].error.empty()) << result[0].error;
  EXPECT_EQ(completed, 1);
}

}  // namespace
}  // namespace perfetto::trace_processor
