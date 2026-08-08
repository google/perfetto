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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "perfetto/ext/protozero/proto_ring_buffer.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using RpcProto = protos::pbzero::TraceProcessorRpc;
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
      rxbuf_.Append(data, len);
      for (;;) {
        auto message = rxbuf_.ReadMessage();
        if (!message.valid())
          break;
        RpcProto::Decoder response(message.start, message.len);
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

TEST(TraceProcessorRpcTest, QueuedCancellationRespondsInFifoPosition) {
  Rpc rpc;
  rpc.EnableThreadedExecution();
  ResponseCollector responses;
  rpc.SetRpcResponseFunction(responses.callback());

  // Keep the TP thread occupied while the tagged cancellable query and its
  // successor enter the FIFO. Cancellation must leave the dead query in place
  // so the successor's response cannot be attributed to it positionally.
  const std::string slow_query =
      "WITH RECURSIVE x(v) AS (VALUES(0) UNION ALL SELECT v + 1 FROM x "
      "WHERE v < 200000) SELECT sum(v) FROM x";
  auto first = QueryRequest(1, slow_query, "blocker", false);
  auto cancelled = QueryRequest(2, "SELECT 2", "target", true);
  auto successor = QueryRequest(3, "SELECT 3", "other", false);
  auto interrupt = InterruptRequest(4, "target");
  rpc.OnRpcRequest(first.data(), first.size());
  rpc.OnRpcRequest(cancelled.data(), cancelled.size());
  rpc.OnRpcRequest(successor.data(), successor.size());
  rpc.OnRpcRequest(interrupt.data(), interrupt.size());

  auto result = responses.WaitFor(3);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_TRUE(result[0].error.empty());
  EXPECT_EQ(result[1].error_code,
            protos::pbzero::QueryResult::ERROR_CODE_INTERRUPTED);
  EXPECT_FALSE(result[1].error.empty());
  EXPECT_TRUE(result[2].error.empty());
  EXPECT_LT(result[0].seq, result[1].seq);
  EXPECT_LT(result[1].seq, result[2].seq);
}

TEST(TraceProcessorRpcTest, CancelQueuedTailNeverInterruptsRunningQuery) {
  Rpc rpc;
  rpc.EnableThreadedExecution();
  ResponseCollector responses;
  rpc.SetRpcResponseFunction(responses.callback());

  const std::string slow_query =
      "WITH RECURSIVE x(v) AS (VALUES(0) UNION ALL SELECT v + 1 FROM x "
      "WHERE v < 200000) SELECT sum(v) FROM x";
  auto running = QueryRequest(1, slow_query, "same-tag", false);
  auto queued = QueryRequest(2, "SELECT 2", "same-tag", true);
  auto interrupt = InterruptRequest(3, "same-tag");
  rpc.OnRpcRequest(running.data(), running.size());
  rpc.OnRpcRequest(queued.data(), queued.size());
  rpc.OnRpcRequest(interrupt.data(), interrupt.size());

  auto result = responses.WaitFor(2);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_TRUE(result[0].error.empty());
  EXPECT_EQ(result[1].error_code,
            protos::pbzero::QueryResult::ERROR_CODE_INTERRUPTED);
}

TEST(TraceProcessorRpcTest, InterruptsRunningCancellableQuery) {
  Rpc rpc;
  rpc.EnableThreadedExecution();
  ResponseCollector responses;
  rpc.SetRpcResponseFunction(responses.callback());

  const std::string slow_query =
      "WITH RECURSIVE x(v) AS (VALUES(0) UNION ALL SELECT v + 1 FROM x "
      "WHERE v < 10000000) SELECT sum(v) FROM x";
  auto running = QueryRequest(1, slow_query, "target", true);
  rpc.OnRpcRequest(running.data(), running.size());
  // Give the TP thread time to pop the request. If this ever becomes flaky,
  // making the query slower only increases the safe interruption window.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  auto interrupt = InterruptRequest(2, "target");
  rpc.OnRpcRequest(interrupt.data(), interrupt.size());

  auto result = responses.WaitFor(1);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].error_code,
            protos::pbzero::QueryResult::ERROR_CODE_INTERRUPTED)
      << result[0].error;
  EXPECT_FALSE(result[0].error.empty());
}

}  // namespace
}  // namespace perfetto::trace_processor
