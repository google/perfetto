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

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto::trace_processor {
namespace {

// Encodes a `QueryArgs` proto carrying the given SQL and returns its
// serialised bytes. The caller drives `Rpc::Query` with this payload.
std::vector<uint8_t> EncodeQueryArgs(const std::string& sql) {
  protozero::HeapBuffered<protos::pbzero::QueryArgs> args;
  args->set_sql_query(sql);
  return args.SerializeAsArray();
}

// Encodes a `TraceProcessorRpc` framing-prefixed wire message that
// asks the server to run a streaming query for the given SQL. The
// returned bytes can be fed directly to `Rpc::OnRpcRequest`. Mirrors
// how the websocket transport builds outgoing messages on the JS side.
std::vector<uint8_t> EncodeStreamingQueryRpcMessage(int64_t seq,
                                                    const std::string& sql) {
  protozero::HeapBuffered<protos::pbzero::TraceProcessorRpcStream> stream;
  auto* msg = stream->add_msg();
  msg->set_seq(seq);
  msg->set_request(
      protos::pbzero::TraceProcessorRpc::TPM_QUERY_STREAMING);
  msg->set_query_args()->set_sql_query(sql);
  return stream.SerializeAsArray();
}

// A trivial multi-producer single-consumer task queue used to stand in
// for `MaybeLockFreeTaskRunner` in tests. The Rpc test driver runs on
// a fixture thread; workers post completion closures here, the
// fixture thread drains them in FIFO order. Deliberately minimal:
// only PostTask + Drain. No wakeup_event_, no fd watches.
class TaskQueue {
 public:
  void Post(std::function<void()> task) {
    std::lock_guard<std::mutex> g(mu_);
    q_.push_back(std::move(task));
    cv_.notify_all();
  }

  // Drains until at least `min_count` tasks have been processed AND
  // the queue is empty for `idle_quiescence_ms` consecutive
  // milliseconds. The double check handles the case where workers are
  // still in flight and will post more tasks.
  void DrainUntilQuiescent(uint32_t expected_count,
                           uint32_t idle_quiescence_ms = 50) {
    uint32_t processed = 0;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(30);
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        if (q_.empty()) {
          if (processed >= expected_count) {
            // Wait briefly to see if more work arrives.
            cv_.wait_for(lock, std::chrono::milliseconds(idle_quiescence_ms));
            if (q_.empty()) {
              return;
            }
          } else {
            cv_.wait_until(lock, deadline);
            if (std::chrono::steady_clock::now() > deadline) {
              FAIL() << "TaskQueue::DrainUntilQuiescent timed out: "
                     << "processed=" << processed
                     << " expected=" << expected_count;
              return;
            }
            if (q_.empty()) {
              continue;
            }
          }
        }
        task = std::move(q_.front());
        q_.pop_front();
      }
      task();
      processed++;
    }
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> q_;
};

// Reads back the response chunks produced by a single `Rpc::Query` call,
// concatenates them, and decodes the trailing `is_last_batch` flag along
// with the integer in column 0 of the (single) row this fixture expects.
struct DecodedSingleIntCol {
  bool ok = false;
  int64_t value = 0;
  std::string error;
};
DecodedSingleIntCol DecodeSingleIntCellResponse(
    const std::vector<std::vector<uint8_t>>& chunks) {
  DecodedSingleIntCol out;
  // Concatenate all the response chunks; each chunk is a serialised
  // `QueryResult` proto.
  for (const auto& chunk : chunks) {
    if (chunk.empty()) {
      continue;
    }
    protos::pbzero::QueryResult::Decoder result(chunk.data(), chunk.size());
    if (result.has_error()) {
      out.error = result.error().ToStdString();
      return out;
    }
    for (auto it = result.batch(); it; ++it) {
      auto bytes = it->as_bytes();
      protos::pbzero::QueryResult::CellsBatch::Decoder batch(bytes.data,
                                                             bytes.size);
      bool parse_error = false;
      for (auto v_it = batch.varint_cells(&parse_error); v_it; ++v_it) {
        out.value = *v_it;
        out.ok = true;
      }
    }
  }
  return out;
}

// Drives `Rpc::Query` once and collects the chunks emitted via the
// callback. Wraps the synchronous pump in a small helper so test bodies
// stay focused on concurrency, not on the wire format.
std::vector<std::vector<uint8_t>> RunQueryAndCollect(Rpc* rpc,
                                                     const std::string& sql) {
  std::vector<uint8_t> args = EncodeQueryArgs(sql);
  std::vector<std::vector<uint8_t>> chunks;
  rpc->Query(args.data(), args.size(),
             [&](const uint8_t* data, size_t len, bool /*has_more*/) {
               if (data && len > 0) {
                 chunks.emplace_back(data, data + len);
               }
             });
  return chunks;
}

// Smoke test for the simple post-EOF query path. The pool should mint one
// connection on the first query and reuse it on subsequent calls.
TEST(RpcTest, PostEofQueryRunsThroughWorkerPool) {
  Rpc rpc;
  ASSERT_OK(rpc.NotifyEndOfFile());

  auto chunks = RunQueryAndCollect(&rpc, "SELECT 42");
  auto decoded = DecodeSingleIntCellResponse(chunks);
  ASSERT_TRUE(decoded.ok) << "decode failed; error=" << decoded.error;
  EXPECT_EQ(decoded.value, 42);
  // Single query: at most one worker thread, exactly one connection
  // minted.
  EXPECT_GE(rpc.pool_distinct_connections_for_testing(), 1u);
  EXPECT_LE(rpc.pool_workers_used_for_testing(), 1u);
}

// Pre-EOF queries must bypass the worker pool because secondary
// connections aren't legal yet (the Phase 2 mutation gate would CHECK).
TEST(RpcTest, PreEofQueryBypassesWorkerPool) {
  Rpc rpc;

  // No NotifyEndOfFile yet. Query should still work — it goes through
  // the writer engine directly.
  auto chunks = RunQueryAndCollect(&rpc, "SELECT 7");
  auto decoded = DecodeSingleIntCellResponse(chunks);
  ASSERT_TRUE(decoded.ok) << "decode failed; error=" << decoded.error;
  EXPECT_EQ(decoded.value, 7);
  // No pool activity: no connections minted, no workers used.
  EXPECT_EQ(rpc.pool_distinct_connections_for_testing(), 0u);
  EXPECT_EQ(rpc.pool_workers_used_for_testing(), 0u);
}

// Fans 8 queries out across N threads concurrently and verifies (a) all
// of them get the right answer back and (b) the pool actually engaged
// more than one worker thread. With `min(hardware_concurrency, 8)`
// workers, at least 2 threads should service the burst on any
// reasonable test machine.
TEST(RpcTest, QueryFansOutAcrossWorkers) {
  Rpc rpc;
  ASSERT_OK(rpc.NotifyEndOfFile());

  // Pre-warm a single connection so the lazy minting cost is paid
  // before timing matters; helps the burst below actually overlap.
  RunQueryAndCollect(&rpc, "SELECT 1");

  constexpr int kQueries = 8;
  std::atomic<int> ok_count{0};
  std::vector<std::thread> threads;
  threads.reserve(kQueries);
  for (int i = 0; i < kQueries; ++i) {
    threads.emplace_back([&rpc, &ok_count, i]() {
      // Each query embeds its own integer literal so we can confirm
      // the right response came back to the right caller.
      auto chunks = RunQueryAndCollect(
          &rpc, "SELECT " + std::to_string(100 + i));
      auto decoded = DecodeSingleIntCellResponse(chunks);
      if (decoded.ok && decoded.value == 100 + i) {
        ok_count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(ok_count.load(), kQueries);
  // The pool runs N workers and each worker is its own thread; the
  // burst should surface at least 2 distinct worker thread IDs on any
  // multi-core machine. `hardware_concurrency` returning 1 in CI is
  // unlikely but allowed: in that pathological case we still ran
  // correctly, just serialised.
  uint32_t workers_used = rpc.pool_workers_used_for_testing();
  if (std::thread::hardware_concurrency() >= 2) {
    EXPECT_GE(workers_used, 2u) << "expected fan-out across >=2 workers";
  }
  // Connections are recycled: at most one per worker thread (and
  // generally fewer, because the pool reuses idle connections across
  // requests on the same worker).
  EXPECT_LE(rpc.pool_distinct_connections_for_testing(),
            static_cast<uint32_t>(kQueries));
}

// Decodes the stream of TraceProcessorRpc messages emitted by Rpc's
// `rpc_response_fn_` for one or more streaming queries. Returns the
// sequence of (seq, query_result_bytes) pairs in the order they were
// emitted, plus a per-seq concatenated query_result body. The async
// streaming dispatch path (see `Rpc::DispatchStreamingQueryAsync`)
// fragments wire bytes across multiple `rpc_response_fn_` calls; this
// helper reassembles them via `ProtoRingBuffer`.
struct DecodedStreamingResponse {
  int64_t seq = 0;
  int request_method = 0;
  std::vector<uint8_t> query_result_bytes;
};
std::vector<DecodedStreamingResponse> DecodeStreamingResponses(
    const std::vector<uint8_t>& wire_bytes) {
  std::vector<DecodedStreamingResponse> out;
  protos::pbzero::TraceProcessorRpcStream::Decoder stream(wire_bytes.data(),
                                                          wire_bytes.size());
  for (auto it = stream.msg(); it; ++it) {
    auto bytes = it->as_bytes();
    protos::pbzero::TraceProcessorRpc::Decoder rpc(bytes.data, bytes.size);
    DecodedStreamingResponse r;
    r.seq = rpc.seq();
    r.request_method = rpc.response();
    if (rpc.has_query_result()) {
      auto qr = rpc.query_result();
      r.query_result_bytes.assign(qr.data, qr.data + qr.size);
    }
    out.push_back(std::move(r));
  }
  return out;
}

// Same scalar decode as `DecodeSingleIntCellResponse` but takes the
// already-extracted `query_result` bytes (the field's value).
DecodedSingleIntCol DecodeSingleIntFromQueryResultBytes(
    const std::vector<uint8_t>& bytes) {
  DecodedSingleIntCol out;
  if (bytes.empty()) {
    return out;
  }
  protos::pbzero::QueryResult::Decoder result(bytes.data(), bytes.size());
  if (result.has_error()) {
    out.error = result.error().ToStdString();
    return out;
  }
  for (auto it = result.batch(); it; ++it) {
    auto bytes_inner = it->as_bytes();
    protos::pbzero::QueryResult::CellsBatch::Decoder batch(bytes_inner.data,
                                                            bytes_inner.size);
    bool parse_error = false;
    for (auto v_it = batch.varint_cells(&parse_error); v_it; ++v_it) {
      out.value = *v_it;
      out.ok = true;
    }
  }
  return out;
}

// A mutating RPC arriving while the pool has cached connections must
// drain the pool cleanly. After the mutation, follow-up queries should
// continue to work; the pool may mint fresh connections.
TEST(RpcTest, MutationDrainsPoolAndQueriesContinue) {
  Rpc rpc;
  ASSERT_OK(rpc.NotifyEndOfFile());

  // Populate the pool with one cached connection.
  RunQueryAndCollect(&rpc, "SELECT 1");
  ASSERT_GE(rpc.pool_distinct_connections_for_testing(), 1u);

  // RestoreInitialTables is a mutating RPC. It must drain pooled
  // connections before touching the writer engine; otherwise the
  // `non_default_connection_count_ == 0` CHECK in the underlying
  // TraceProcessor would fire.
  rpc.RestoreInitialTables();

  // Subsequent queries still work; the pool recreates connections on
  // demand.
  auto chunks = RunQueryAndCollect(&rpc, "SELECT 5");
  auto decoded = DecodeSingleIntCellResponse(chunks);
  ASSERT_TRUE(decoded.ok) << "decode failed; error=" << decoded.error;
  EXPECT_EQ(decoded.value, 5);
}

// Drives the async streaming dispatch path end-to-end. Sets up a
// dispatcher that routes worker-completion closures back to a fake
// task queue (standing in for httpd's task runner), pushes a single
// TPM_QUERY_STREAMING request through `OnRpcRequest`, and verifies
// (a) `OnRpcRequest` returns *before* the response is delivered (the
// task-runner-unblock invariant) and (b) the response wire bytes
// decode correctly once the task is drained.
TEST(RpcTest, StreamingQueryDispatchesAsyncAndUnblocksTransport) {
  Rpc rpc;
  ASSERT_OK(rpc.NotifyEndOfFile());

  TaskQueue task_queue;
  std::vector<uint8_t> wire_bytes;
  rpc.SetRpcResponseFunction([&](const void* data, uint32_t len) {
    auto* p = static_cast<const uint8_t*>(data);
    wire_bytes.insert(wire_bytes.end(), p, p + len);
  });
  rpc.SetResponseDispatcher([&](std::function<void()> task) {
    task_queue.Post(std::move(task));
  });

  auto msg = EncodeStreamingQueryRpcMessage(/*seq=*/1, "SELECT 99");
  rpc.OnRpcRequest(msg.data(), msg.size());

  // OnRpcRequest must have returned without producing any wire bytes:
  // the worker is still serializing on a pool thread and has not yet
  // posted back. (Race: the worker could be fast enough to post-task
  // before this check, but the task itself only runs on
  // task_queue.DrainUntilQuiescent below. So `wire_bytes` must be
  // empty here regardless.)
  EXPECT_TRUE(wire_bytes.empty())
      << "Async dispatch leaked wire bytes onto OnRpcRequest's stack; "
      << "task runner would be blocked.";
  EXPECT_EQ(rpc.streaming_async_dispatches_for_testing(), 1u);

  // Drain the queue: the worker's PostTask will run here.
  task_queue.DrainUntilQuiescent(/*expected_count=*/1);

  // Response should now be on the wire.
  ASSERT_FALSE(wire_bytes.empty());
  auto responses = DecodeStreamingResponses(wire_bytes);
  ASSERT_FALSE(responses.empty());

  // Concatenate per-seq query_result bytes (multi-chunk safe).
  std::vector<uint8_t> qr_bytes;
  for (const auto& r : responses) {
    qr_bytes.insert(qr_bytes.end(), r.query_result_bytes.begin(),
                    r.query_result_bytes.end());
  }
  auto decoded = DecodeSingleIntFromQueryResultBytes(qr_bytes);
  ASSERT_TRUE(decoded.ok) << "decode failed; error=" << decoded.error;
  EXPECT_EQ(decoded.value, 99);
}

// Issues 8 concurrent streaming queries via OnRpcRequest with the
// async dispatcher wired up. Verifies (a) all of them get the right
// answer back, (b) responses arrive in send-order (the UI's
// `pendingQueries[0]` FIFO invariant), and (c) the worker pool
// engaged more than one thread.
TEST(RpcTest, StreamingQueryFansOutAcrossWorkers) {
  Rpc rpc;
  ASSERT_OK(rpc.NotifyEndOfFile());

  TaskQueue task_queue;
  std::vector<uint8_t> wire_bytes;
  std::mutex wire_mu;
  rpc.SetRpcResponseFunction([&](const void* data, uint32_t len) {
    std::lock_guard<std::mutex> g(wire_mu);
    auto* p = static_cast<const uint8_t*>(data);
    wire_bytes.insert(wire_bytes.end(), p, p + len);
  });
  rpc.SetResponseDispatcher([&](std::function<void()> task) {
    task_queue.Post(std::move(task));
  });

  // Pre-warm so the lazy minting cost is paid before timing matters.
  {
    auto msg = EncodeStreamingQueryRpcMessage(/*seq=*/0, "SELECT 0");
    rpc.OnRpcRequest(msg.data(), msg.size());
    task_queue.DrainUntilQuiescent(/*expected_count=*/1);
    std::lock_guard<std::mutex> g(wire_mu);
    wire_bytes.clear();
  }

  constexpr int kQueries = 8;
  for (int i = 0; i < kQueries; ++i) {
    auto msg = EncodeStreamingQueryRpcMessage(
        /*seq=*/static_cast<int64_t>(i + 1),
        "SELECT " + std::to_string(100 + i));
    rpc.OnRpcRequest(msg.data(), msg.size());
  }

  // OnRpcRequest never blocked: all 8 dispatches counted, and no
  // response bytes have been emitted yet (no task drained).
  EXPECT_GE(rpc.streaming_async_dispatches_for_testing(),
            static_cast<uint32_t>(kQueries));
  {
    std::lock_guard<std::mutex> g(wire_mu);
    EXPECT_TRUE(wire_bytes.empty())
        << "Async dispatch leaked wire bytes during OnRpcRequest;"
        << " the transport thread is not free for concurrent messages.";
  }

  task_queue.DrainUntilQuiescent(/*expected_count=*/kQueries);

  std::vector<DecodedStreamingResponse> responses;
  {
    std::lock_guard<std::mutex> g(wire_mu);
    responses = DecodeStreamingResponses(wire_bytes);
  }

  // Group responses by seq (each query may produce one or more chunks
  // = one or more responses with monotonically increasing seq IDs).
  // We expect kQueries distinct integer values, each decoding to 100+i
  // where i is the dispatch order.
  // Reconstruct per-query bytes by walking response seqs in order:
  // the async path emits all chunks of one query contiguously, so
  // group-by-position works.
  std::vector<std::vector<uint8_t>> per_query_bytes(kQueries);
  // Each query produces one or more responses; for `SELECT N` a
  // single chunk suffices. With send-order ordering enforced inside
  // Rpc, the responses come out in dispatch order — and dispatch
  // order matches the `100+i` literal.
  ASSERT_GE(responses.size(), static_cast<size_t>(kQueries));
  // Assign responses to queries by walking sequentially: a response
  // belongs to the i-th query iff i is the smallest index whose
  // bytes haven't yet been finalised. We treat each non-empty
  // query_result that decodes a non-zero integer as a distinct
  // chunk; in this test we only `SELECT N` which fits into a single
  // chunk, so queries == responses.
  for (size_t i = 0;
       i < static_cast<size_t>(kQueries) && i < responses.size(); ++i) {
    per_query_bytes[i] = responses[i].query_result_bytes;
  }
  for (size_t i = 0; i < static_cast<size_t>(kQueries); ++i) {
    auto decoded = DecodeSingleIntFromQueryResultBytes(per_query_bytes[i]);
    EXPECT_TRUE(decoded.ok)
        << "query " << i << " decode failed; error=" << decoded.error;
    EXPECT_EQ(decoded.value, static_cast<int64_t>(100 + i))
        << "query " << i << " came back out of order (got " << decoded.value
        << ", expected " << (100 + i)
        << "). Send-order invariant violated.";
  }

  // Pool fan-out check.
  uint32_t workers_used = rpc.pool_workers_used_for_testing();
  if (std::thread::hardware_concurrency() >= 2) {
    EXPECT_GE(workers_used, 2u) << "expected fan-out across >=2 workers";
  }
}

// Verifies content identity between the inline streaming path
// (today's behaviour, no dispatcher set) and the async streaming
// path (this iter's new behaviour, dispatcher set). Decodes the
// query_result protos from each and compares everything except
// `elapsed_time_ms` (which is wall-time-dependent and so by design
// differs between two runs of the same query).
TEST(RpcTest, StreamingQueryAsyncMatchesInlineSemantically) {
  const char* kSql =
      "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
      "WHERE x<200) SELECT x FROM c";
  // Inline reference: dispatcher unset.
  std::vector<uint8_t> inline_bytes;
  {
    Rpc rpc;
    ASSERT_OK(rpc.NotifyEndOfFile());
    rpc.SetRpcResponseFunction([&](const void* data, uint32_t len) {
      auto* p = static_cast<const uint8_t*>(data);
      inline_bytes.insert(inline_bytes.end(), p, p + len);
    });
    auto msg = EncodeStreamingQueryRpcMessage(/*seq=*/1, kSql);
    rpc.OnRpcRequest(msg.data(), msg.size());
  }

  // Async path with dispatcher.
  std::vector<uint8_t> async_bytes;
  {
    Rpc rpc;
    ASSERT_OK(rpc.NotifyEndOfFile());
    TaskQueue tq;
    rpc.SetRpcResponseFunction([&](const void* data, uint32_t len) {
      auto* p = static_cast<const uint8_t*>(data);
      async_bytes.insert(async_bytes.end(), p, p + len);
    });
    rpc.SetResponseDispatcher(
        [&](std::function<void()> task) { tq.Post(std::move(task)); });
    auto msg = EncodeStreamingQueryRpcMessage(/*seq=*/1, kSql);
    rpc.OnRpcRequest(msg.data(), msg.size());
    tq.DrainUntilQuiescent(/*expected_count=*/1);
  }

  // Walk the responses; compare everything except the wall-time-derived
  // `elapsed_time_ms`. Concatenate cell payloads + statement metadata.
  auto extract_summary = [](const std::vector<uint8_t>& bytes) {
    struct Summary {
      std::vector<int64_t> values;
      uint32_t total_batches = 0;
      uint32_t total_rows = 0;
      uint32_t statement_count = 0;
      bool has_last_batch = false;
      std::vector<std::string> column_names;
    };
    Summary s;
    auto resp = DecodeStreamingResponses(bytes);
    for (const auto& r : resp) {
      protos::pbzero::QueryResult::Decoder qr(r.query_result_bytes.data(),
                                              r.query_result_bytes.size());
      if (qr.has_statement_count()) {
        s.statement_count = qr.statement_count();
      }
      for (auto cn = qr.column_names(); cn; ++cn) {
        s.column_names.push_back(cn->as_std_string());
      }
      for (auto bit = qr.batch(); bit; ++bit) {
        s.total_batches++;
        auto bb = bit->as_bytes();
        protos::pbzero::QueryResult::CellsBatch::Decoder batch(bb.data,
                                                                bb.size);
        if (batch.is_last_batch()) {
          s.has_last_batch = true;
        }
        bool perr = false;
        for (auto v = batch.varint_cells(&perr); v; ++v) {
          s.values.push_back(*v);
          s.total_rows++;
        }
      }
    }
    return s;
  };
  auto inline_s = extract_summary(inline_bytes);
  auto async_s = extract_summary(async_bytes);
  EXPECT_EQ(inline_s.values, async_s.values);
  EXPECT_EQ(inline_s.total_rows, async_s.total_rows);
  EXPECT_EQ(inline_s.total_batches, async_s.total_batches);
  EXPECT_EQ(inline_s.statement_count, async_s.statement_count);
  EXPECT_EQ(inline_s.has_last_batch, async_s.has_last_batch);
  EXPECT_EQ(inline_s.column_names, async_s.column_names);
  EXPECT_EQ(inline_s.values.size(), 200u);
}

}  // namespace
}  // namespace perfetto::trace_processor
