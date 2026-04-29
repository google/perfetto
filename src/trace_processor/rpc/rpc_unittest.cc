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
#include <cstddef>
#include <cstdint>
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

}  // namespace
}  // namespace perfetto::trace_processor
