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

// Encodes a `QueryArgs` proto carrying the given SQL.
std::vector<uint8_t> EncodeQueryArgs(const std::string& sql) {
  protozero::HeapBuffered<protos::pbzero::QueryArgs> args;
  args->set_sql_query(sql);
  return args.SerializeAsArray();
}

// Encodes a `TraceProcessorRpcStream`-framed `TPM_QUERY_STREAMING`
// request. If `tag` is non-empty it is set on the QueryArgs (otherwise
// the request goes through the untagged-stream path).
std::vector<uint8_t> EncodeStreamingQueryRpcMessage(
    int64_t seq, const std::string& sql, const std::string& tag = {}) {
  protozero::HeapBuffered<protos::pbzero::TraceProcessorRpcStream> stream;
  auto* msg = stream->add_msg();
  msg->set_seq(seq);
  msg->set_request(protos::pbzero::TraceProcessorRpc::TPM_QUERY_STREAMING);
  auto* args = msg->set_query_args();
  args->set_sql_query(sql);
  if (!tag.empty()) {
    args->set_tag(tag);
  }
  return stream.SerializeAsArray();
}

// Trivial MPSC task queue standing in for `MaybeLockFreeTaskRunner`.
// Workers post completion closures here; the test driver thread drains
// them in FIFO order.
class TaskQueue {
 public:
  void Post(std::function<void()> task) {
    std::lock_guard<std::mutex> g(mu_);
    q_.push_back(std::move(task));
    cv_.notify_all();
  }

  // Drains until at least `expected_count` tasks have been processed
  // AND the queue is briefly empty, with a 30s safety deadline.
  void DrainUntilQuiescent(uint32_t expected_count,
                           uint32_t idle_quiescence_ms = 50) {
    uint32_t processed = 0;
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        if (q_.empty()) {
          if (processed >= expected_count) {
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

// Decodes a `TraceProcessorRpcStream` byte stream into a sequence of
// `(seq, query_result_bytes)` records, one per RpcProto on the wire.
struct DecodedStreamingResponse {
  int64_t seq = 0;
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
    if (rpc.has_query_result()) {
      auto qr = rpc.query_result();
      r.query_result_bytes.assign(qr.data, qr.data + qr.size);
    }
    out.push_back(std::move(r));
  }
  return out;
}

// Decodes the integer in column 0 of the (single) row from a
// concatenated `query_result` byte buffer. Returns 0 on decode error
// and flags `ok=false`.
struct DecodedSingleInt {
  bool ok = false;
  int64_t value = 0;
  std::string error;
};
DecodedSingleInt DecodeSingleInt(const std::vector<uint8_t>& bytes) {
  DecodedSingleInt out;
  if (bytes.empty()) {
    return out;
  }
  protos::pbzero::QueryResult::Decoder result(bytes.data(), bytes.size());
  if (result.has_error()) {
    out.error = result.error().ToStdString();
    return out;
  }
  for (auto it = result.batch(); it; ++it) {
    auto inner = it->as_bytes();
    protos::pbzero::QueryResult::CellsBatch::Decoder batch(inner.data,
                                                           inner.size);
    bool perr = false;
    for (auto v_it = batch.varint_cells(&perr); v_it; ++v_it) {
      out.value = *v_it;
      out.ok = true;
    }
  }
  return out;
}

// Convenience overload for the sync `Rpc::Query` callback shape: each
// emitted chunk is a serialised QueryResult; we concatenate then
// decode.
DecodedSingleInt DecodeSingleInt(
    const std::vector<std::vector<uint8_t>>& chunks) {
  std::vector<uint8_t> all;
  for (const auto& c : chunks) {
    all.insert(all.end(), c.begin(), c.end());
  }
  return DecodeSingleInt(all);
}

// Drives the synchronous `Rpc::Query` path and returns the chunks
// emitted via the callback.
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

// Fixture for the async streaming dispatch tests. Wires the test
// harness's task queue up to `Rpc::SetResponseDispatcher` and
// accumulates wire bytes (under lock) so concurrent worker threads
// can append safely.
class RpcStreamingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK(rpc_.NotifyEndOfFile());
    rpc_.SetRpcResponseFunction([this](const void* data, uint32_t len) {
      std::lock_guard<std::mutex> g(wire_mu_);
      auto* p = static_cast<const uint8_t*>(data);
      wire_bytes_.insert(wire_bytes_.end(), p, p + len);
    });
    rpc_.SetResponseDispatcher(
        [this](std::function<void()> task) { tq_.Post(std::move(task)); });
  }

  // Submits one streaming query. Empty `tag` exercises the untagged-
  // stream path; non-empty exercises tag-affine dispatch.
  void Submit(int64_t seq, const std::string& sql,
              const std::string& tag = {}) {
    auto msg = EncodeStreamingQueryRpcMessage(seq, sql, tag);
    rpc_.OnRpcRequest(msg.data(), msg.size());
  }

  void Drain(uint32_t expected_count) {
    tq_.DrainUntilQuiescent(expected_count);
  }

  std::vector<uint8_t> WireBytesSnapshot() {
    std::lock_guard<std::mutex> g(wire_mu_);
    return wire_bytes_;
  }
  void ClearWire() {
    std::lock_guard<std::mutex> g(wire_mu_);
    wire_bytes_.clear();
  }
  bool WireIsEmpty() {
    std::lock_guard<std::mutex> g(wire_mu_);
    return wire_bytes_.empty();
  }

  Rpc rpc_;
  TaskQueue tq_;
  std::mutex wire_mu_;
  std::vector<uint8_t> wire_bytes_;
};

// Post-EOF query: the pool mints one connection and reuses it.
TEST(RpcTest, PostEofQueryRunsThroughWorkerPool) {
  Rpc rpc;
  ASSERT_OK(rpc.NotifyEndOfFile());

  auto decoded = DecodeSingleInt(RunQueryAndCollect(&rpc, "SELECT 42"));
  ASSERT_TRUE(decoded.ok) << "decode failed; error=" << decoded.error;
  EXPECT_EQ(decoded.value, 42);
  EXPECT_GE(rpc.pool_distinct_connections_for_testing(), 1u);
  EXPECT_LE(rpc.pool_workers_used_for_testing(), 1u);
}

// Pre-EOF queries bypass the worker pool (secondary connections
// aren't legal yet — TraceProcessor would CHECK).
TEST(RpcTest, PreEofQueryBypassesWorkerPool) {
  Rpc rpc;
  auto decoded = DecodeSingleInt(RunQueryAndCollect(&rpc, "SELECT 7"));
  ASSERT_TRUE(decoded.ok) << "decode failed; error=" << decoded.error;
  EXPECT_EQ(decoded.value, 7);
  EXPECT_EQ(rpc.pool_distinct_connections_for_testing(), 0u);
  EXPECT_EQ(rpc.pool_workers_used_for_testing(), 0u);
}

// Sync `Rpc::Query` from N threads fans out across worker threads.
TEST(RpcTest, QueryFansOutAcrossWorkers) {
  Rpc rpc;
  ASSERT_OK(rpc.NotifyEndOfFile());
  RunQueryAndCollect(&rpc, "SELECT 1");  // pre-warm

  constexpr int kQueries = 8;
  std::atomic<int> ok_count{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kQueries; ++i) {
    threads.emplace_back([&rpc, &ok_count, i]() {
      auto decoded = DecodeSingleInt(
          RunQueryAndCollect(&rpc, "SELECT " + std::to_string(100 + i)));
      if (decoded.ok && decoded.value == 100 + i) {
        ok_count.fetch_add(1);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(ok_count.load(), kQueries);
  if (std::thread::hardware_concurrency() >= 2) {
    EXPECT_GE(rpc.pool_workers_used_for_testing(), 2u);
  }
  EXPECT_LE(rpc.pool_distinct_connections_for_testing(),
            static_cast<uint32_t>(kQueries));
}

// `OnRpcRequest` returns before the response is delivered; the
// response decodes correctly once the dispatcher's task is drained.
TEST_F(RpcStreamingTest, DispatchesAsyncAndUnblocksTransport) {
  Submit(/*seq=*/1, "SELECT 99");
  EXPECT_TRUE(WireIsEmpty())
      << "Async dispatch leaked wire bytes onto OnRpcRequest's stack; "
      << "task runner would be blocked.";

  Drain(/*expected_count=*/1);

  auto responses = DecodeStreamingResponses(WireBytesSnapshot());
  ASSERT_FALSE(responses.empty());
  std::vector<uint8_t> qr;
  for (const auto& r : responses) {
    qr.insert(qr.end(), r.query_result_bytes.begin(),
              r.query_result_bytes.end());
  }
  auto decoded = DecodeSingleInt(qr);
  ASSERT_TRUE(decoded.ok) << "decode failed; error=" << decoded.error;
  EXPECT_EQ(decoded.value, 99);
}

// 8 concurrent async streaming queries fan out across workers and
// arrive in send-order (pendingQueries[0] FIFO).
TEST_F(RpcStreamingTest, FansOutAcrossWorkers) {
  // Pre-warm so the lazy minting cost is paid before timing matters.
  Submit(/*seq=*/0, "SELECT 0");
  Drain(/*expected_count=*/1);
  ClearWire();

  constexpr int kQueries = 8;
  for (int i = 0; i < kQueries; ++i) {
    Submit(/*seq=*/i + 1, "SELECT " + std::to_string(100 + i));
  }
  EXPECT_TRUE(WireIsEmpty())
      << "Async dispatch leaked wire bytes during OnRpcRequest;"
      << " transport thread is not free for concurrent messages.";

  Drain(/*expected_count=*/kQueries);

  auto responses = DecodeStreamingResponses(WireBytesSnapshot());
  ASSERT_GE(responses.size(), static_cast<size_t>(kQueries));
  // Responses come out in dispatch order; for `SELECT N` each query
  // produces exactly one chunk so position equals query index.
  for (size_t i = 0; i < static_cast<size_t>(kQueries); ++i) {
    auto decoded = DecodeSingleInt(responses[i].query_result_bytes);
    EXPECT_TRUE(decoded.ok)
        << "query " << i << " decode failed; error=" << decoded.error;
    EXPECT_EQ(decoded.value, static_cast<int64_t>(100 + i))
        << "query " << i << " came back out of order.";
  }
  if (std::thread::hardware_concurrency() >= 2) {
    EXPECT_GE(rpc_.pool_workers_used_for_testing(), 2u);
  }
}

// Inline (no dispatcher) and async streaming paths must produce
// identical responses (modulo `elapsed_time_ms`, which is wall-time
// dependent).
TEST(RpcTest, StreamingQueryAsyncMatchesInlineSemantically) {
  const char* kSql =
      "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
      "WHERE x<200) SELECT x FROM c";
  std::vector<uint8_t> inline_bytes, async_bytes;
  {
    Rpc rpc;
    ASSERT_OK(rpc.NotifyEndOfFile());
    rpc.SetRpcResponseFunction([&](const void* data, uint32_t len) {
      auto* p = static_cast<const uint8_t*>(data);
      inline_bytes.insert(inline_bytes.end(), p, p + len);
    });
    auto msg = EncodeStreamingQueryRpcMessage(1, kSql);
    rpc.OnRpcRequest(msg.data(), msg.size());
  }
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
    auto msg = EncodeStreamingQueryRpcMessage(1, kSql);
    rpc.OnRpcRequest(msg.data(), msg.size());
    tq.DrainUntilQuiescent(1);
  }

  // Walk the responses; compare everything except the wall-time-
  // derived `elapsed_time_ms`. Concatenate cell payloads + statement
  // metadata.
  struct Summary {
    std::vector<int64_t> values;
    uint32_t total_batches = 0;
    uint32_t total_rows = 0;
    uint32_t statement_count = 0;
    bool has_last_batch = false;
    std::vector<std::string> column_names;
  };
  auto summarise = [](const std::vector<uint8_t>& bytes) {
    Summary s;
    for (const auto& r : DecodeStreamingResponses(bytes)) {
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
  auto a = summarise(inline_bytes);
  auto b = summarise(async_bytes);
  EXPECT_EQ(a.values, b.values);
  EXPECT_EQ(a.total_rows, b.total_rows);
  EXPECT_EQ(a.total_batches, b.total_batches);
  EXPECT_EQ(a.statement_count, b.statement_count);
  EXPECT_EQ(a.has_last_batch, b.has_last_batch);
  EXPECT_EQ(a.column_names, b.column_names);
  EXPECT_EQ(a.values.size(), 200u);
}

// A query slow enough that the test-driver thread can fire several of
// them before any worker finishes — required for the fan-out test to
// observe pool growth. A trivial `SELECT 1` runs in microseconds,
// faster than the driver's next OnRpcRequest, so all queries end up
// serialising on the first connection.
constexpr const char* kSlowQuery =
    "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
    "WHERE x<5000) SELECT count(*) FROM c";

// Same-tag streaming queries serialise on a single connection (the
// affinity hit). Different-tag queries fan out across connections.
TEST_F(RpcStreamingTest, SameTagSerialisesDifferentTagsFanOut) {
  if (std::thread::hardware_concurrency() < 2) {
    GTEST_SKIP() << "fan-out test needs >= 2 cores";
  }

  // Same-tag run: 4 queries with tag "A". Only one tag-slot is in
  // flight at a time, so each subsequent query lands on the same
  // (now-released, affined) connection. The pool never grows because
  // pool_free_ is non-empty between dispatches.
  int64_t seq = 1;
  constexpr int kQueriesPerTag = 4;
  for (int i = 0; i < kQueriesPerTag; ++i) {
    Submit(seq++, kSlowQuery, /*tag=*/"A");
  }
  Drain(kQueriesPerTag);
  EXPECT_EQ(rpc_.tag_slots_size_for_testing(), 0u);
  EXPECT_EQ(rpc_.affinity_size_for_testing(), 1u);
  EXPECT_TRUE(rpc_.has_affinity_for_testing("A"));
  EXPECT_EQ(rpc_.pool_distinct_connections_for_testing(), 1u);

  // Different-tag run: 8 queries, 8 distinct tags. Pool grows because
  // the driver outpaces worker completion.
  ClearWire();
  constexpr int kDistinctTags = 8;
  for (int i = 0; i < kDistinctTags; ++i) {
    Submit(seq++, kSlowQuery, /*tag=*/"T" + std::to_string(i));
  }
  Drain(kDistinctTags);
  EXPECT_EQ(rpc_.tag_slots_size_for_testing(), 0u);
  // 1 ("A") + 8 distinct tags = 9 affinity entries.
  EXPECT_EQ(rpc_.affinity_size_for_testing(), 9u);
  EXPECT_GE(rpc_.pool_distinct_connections_for_testing(), 2u);
}

// Empty tag is the "untagged stream" — all empty-tag queries share
// one tag-slot and one affined connection.
TEST_F(RpcStreamingTest, UntaggedQueriesShareOneConnection) {
  constexpr int kQueries = 8;
  for (int i = 0; i < kQueries; ++i) {
    Submit(/*seq=*/i + 1, "SELECT " + std::to_string(300 + i));
  }
  Drain(kQueries);
  EXPECT_EQ(rpc_.affinity_size_for_testing(), 1u);
  EXPECT_TRUE(rpc_.has_affinity_for_testing(""));
  EXPECT_EQ(rpc_.tag_slots_size_for_testing(), 0u);
}

// LRU eviction kicks in when more distinct tags arrive than
// `kMaxAffinityEntries`. The map size must never exceed the cap.
TEST_F(RpcStreamingTest, AffinityLRUEvictsAtCap) {
  constexpr int kTagsToInsert = 100;  // > kMaxAffinityEntries (= 64)
  for (int i = 0; i < kTagsToInsert; ++i) {
    Submit(/*seq=*/i + 1, "SELECT " + std::to_string(i),
           /*tag=*/"unique_tag_" + std::to_string(i));
    // Sequential dispatch keeps LRU order = insertion order.
    Drain(/*expected_count=*/1);
  }
  EXPECT_EQ(rpc_.affinity_size_for_testing(), 64u);
  EXPECT_FALSE(rpc_.has_affinity_for_testing("unique_tag_0"))
      << "oldest tag should have been LRU-evicted";
  EXPECT_TRUE(rpc_.has_affinity_for_testing(
      "unique_tag_" + std::to_string(kTagsToInsert - 1)));
}

}  // namespace
}  // namespace perfetto::trace_processor
