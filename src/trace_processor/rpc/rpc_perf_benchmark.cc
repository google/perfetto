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

// End-to-end perf benchmark for the multi-connection RPC worker pool.
//
// `*_PoolOff` runs the synchronous inline path (no dispatcher; queries
// execute on the caller thread one at a time). `*_PoolOn_*` variants
// wire a dispatcher and route post-EOF streaming queries through the
// worker pool. The three pool-on variants — `Untagged`, `SameTag`,
// `DistinctTags` — exercise the tag-affine dispatch contract: empty
// and same tag funnel through one connection (untagged-stream
// semantics), distinct tags fan out across the pool. Speedup =
// PoolOff / PoolOn_DistinctTags.
//
// Each iteration measures wall-time from "first query dispatched" to
// "last response delivered" plus three diagnostic counters:
//   worker_parallelism  — sum_per_query_sql_exec_ns / wall_ns,
//                         caps at #pool_workers.
//   dispatcher_fraction — time the transport thread spent in the
//                         drain closure / wall_ns; ≈1.0 means the
//                         single-threaded dispatcher is the bottleneck.
//   parallel_ceiling    — (PoolOff only) sum_per_query / max_per_query;
//                         the Amdahl-bound speedup the pool can ever
//                         hit on this workload.
//
// Two workloads:
//   `WorkloadQueries`         — UI-style burst (uneven costs; the
//                               JOIN+GROUP BY query dominates wall).
//   `BalancedWorkloadQueries` — same shape, even costs; isolates
//                               dispatch fan-out from query variance.
//   `CpuOnlyQueries`          — recursive-CTE only, no shared trace
//                               state.
//
// Trace fixture: `test/data/android_postboot_unlock.pftrace` (~18 MB
// — moderately large; small enough that the load step doesn't dwarf
// the query step). The benchmark fails (skips with error) if the
// fixture is missing.

#include <benchmark/benchmark.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_processor/rpc/rpc.h"

#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto::trace_processor {
namespace {

constexpr const char* kTraceFixture =
    "test/data/android_postboot_unlock.pftrace";

// The SELECTs below approximate the post-EOF query flurry the UI
// issues on trace load: a mix of GROUP-BYs and ORDER-BY-LIMIT scans
// over the busiest tables. Group-bys with modest result sets are a
// closer match to what UI track-summarisation queries look like at
// trace-load time than count(*) probes (which run sub-millisecond and
// don't show the pool's parallelism win).
const std::vector<std::string>& WorkloadQueries() {
  static const std::vector<std::string> kQueries = {
      "SELECT name, count(*) c, sum(dur) s FROM slice "
      "GROUP BY name ORDER BY s DESC LIMIT 100",
      "SELECT category, count(*) FROM slice "
      "GROUP BY category ORDER BY 2 DESC LIMIT 100",
      "SELECT utid, count(*) FROM thread_state "
      "GROUP BY utid ORDER BY 2 DESC LIMIT 100",
      "SELECT state, count(*), sum(dur) FROM thread_state "
      "GROUP BY state ORDER BY 3 DESC",
      "SELECT cpu, count(*), sum(dur) FROM sched "
      "GROUP BY cpu ORDER BY cpu",
      "SELECT track_id, count(*) FROM counter "
      "GROUP BY track_id ORDER BY 2 DESC LIMIT 100",
      "SELECT key, count(*) FROM args "
      "GROUP BY key ORDER BY 2 DESC LIMIT 100",
      "SELECT depth, count(*), sum(dur) FROM slice "
      "GROUP BY depth ORDER BY depth",
      "SELECT track_id, count(*), sum(dur) FROM slice "
      "GROUP BY track_id ORDER BY 3 DESC LIMIT 100",
      "SELECT ts, dur, name, category FROM slice "
      "WHERE dur > 100000 ORDER BY dur DESC LIMIT 1000",
      "SELECT ts, dur, name FROM slice "
      "WHERE name GLOB '*draw*' ORDER BY ts LIMIT 5000",
      "SELECT thread.name, count(*) c FROM thread_state "
      "JOIN thread USING (utid) GROUP BY thread.name "
      "ORDER BY c DESC LIMIT 100",
  };
  return kQueries;
}

// 10 same-shape slice GROUP BYs. With every query about the same cost
// (sum/max ≈ 5), the achieved speedup is a clean read of how well
// dispatch fans out across worker connections — the Amdahl variance
// in `WorkloadQueries` is factored out.
const std::vector<std::string>& BalancedWorkloadQueries() {
  static const std::vector<std::string> kQueries = {
      "SELECT name, count(*) c, sum(dur) s FROM slice "
      "GROUP BY name ORDER BY s DESC LIMIT 100",
      "SELECT category, count(*) c, sum(dur) s FROM slice "
      "GROUP BY category ORDER BY s DESC LIMIT 100",
      "SELECT depth, count(*) c, sum(dur) s FROM slice "
      "GROUP BY depth ORDER BY s DESC LIMIT 100",
      "SELECT track_id, count(*) c, sum(dur) s FROM slice "
      "GROUP BY track_id ORDER BY s DESC LIMIT 100",
      "SELECT name, max(dur) c, min(dur) s FROM slice "
      "GROUP BY name ORDER BY s DESC LIMIT 100",
      "SELECT category, max(dur) c, min(dur) s FROM slice "
      "GROUP BY category ORDER BY s DESC LIMIT 100",
      "SELECT depth, max(dur) c, min(dur) s FROM slice "
      "GROUP BY depth ORDER BY s DESC LIMIT 100",
      "SELECT track_id, max(dur) c, min(dur) s FROM slice "
      "GROUP BY track_id ORDER BY s DESC LIMIT 100",
      "SELECT name, count(*) c FROM slice "
      "WHERE dur > 1000 GROUP BY name ORDER BY c DESC LIMIT 100",
      "SELECT category, count(*) c FROM slice "
      "WHERE dur > 1000 GROUP BY category ORDER BY c DESC LIMIT 100",
  };
  return kQueries;
}

// 8 copies of a 50K-row recursive CTE. Each query is independent and
// touches no trace tables; running them all gives the dispatch layer
// 8 distinct-tag tasks to fan out across the worker pool. The
// headline reading is "what's the dispatch ceiling when SQL execution
// itself touches no shared state".
const std::vector<std::string>& CpuOnlyQueries() {
  static const std::vector<std::string> kQueries(
      8,
      "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
      "WHERE x<50000) SELECT sum(x*x*x) FROM c");
  return kQueries;
}

// Encodes a `TraceProcessorRpcStream`-framed `TPM_QUERY_STREAMING`
// request. Empty `tag` exercises the untagged-stream path.
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

// Counts `TPM_QUERY_STREAMING` responses where `is_last_batch=true`
// — the number of *queries* that completed (not chunks).
size_t CountQueriesCompleted(const std::vector<uint8_t>& wire_bytes) {
  size_t completed = 0;
  protos::pbzero::TraceProcessorRpcStream::Decoder stream(wire_bytes.data(),
                                                          wire_bytes.size());
  for (auto it = stream.msg(); it; ++it) {
    auto bytes = it->as_bytes();
    protos::pbzero::TraceProcessorRpc::Decoder rpc(bytes.data, bytes.size);
    if (!rpc.has_query_result()) {
      continue;
    }
    auto qr = rpc.query_result();
    protos::pbzero::QueryResult::Decoder result(qr.data, qr.size);
    for (auto b_it = result.batch(); b_it; ++b_it) {
      auto b_bytes = b_it->as_bytes();
      protos::pbzero::QueryResult::CellsBatch::Decoder batch(b_bytes.data,
                                                              b_bytes.size);
      if (batch.is_last_batch()) {
        completed++;
      }
    }
  }
  return completed;
}

// Trivial MPSC task queue standing in for `MaybeLockFreeTaskRunner`.
class TaskQueue {
 public:
  void Post(std::function<void()> task) {
    std::lock_guard<std::mutex> g(mu_);
    q_.push_back(std::move(task));
    cv_.notify_all();
  }

  bool DrainUntil(std::function<bool()> predicate,
                  std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      if (predicate()) {
        return true;
      }
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        if (q_.empty()) {
          if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            return predicate();
          }
          if (q_.empty()) {
            continue;
          }
        }
        task = std::move(q_.front());
        q_.pop_front();
      }
      task();
    }
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> q_;
};

// Loads the fixture trace into `rpc`. Skips the benchmark with a
// readable error when the fixture is missing.
bool LoadTraceInto(Rpc* rpc, benchmark::State& bstate) {
  std::string contents;
  if (!base::ReadFile(kTraceFixture, &contents)) {
    bstate.SkipWithError("Test data missing. Please ensure "
                         "test/data/android_postboot_unlock.pftrace exists.");
    return false;
  }
  if (auto s = rpc->Parse(reinterpret_cast<const uint8_t*>(contents.data()),
                          contents.size());
      !s.ok()) {
    bstate.SkipWithError(("Rpc::Parse failed: " + s.message()).c_str());
    return false;
  }
  if (auto s = rpc->NotifyEndOfFile(); !s.ok()) {
    bstate.SkipWithError(
        ("Rpc::NotifyEndOfFile failed: " + s.message()).c_str());
    return false;
  }
  return true;
}

enum class TagStrategy { kEmpty, kSameTag, kDistinctTags };

std::string TagFor(TagStrategy s, size_t i) {
  switch (s) {
    case TagStrategy::kEmpty:
      return {};
    case TagStrategy::kSameTag:
      return "shared";
    case TagStrategy::kDistinctTags:
      return "tag_" + std::to_string(i);
  }
  return {};
}

// Pool ON: queries dispatch async through a wired-up dispatcher.
// `tag_strategy` controls how the burst maps to RPC tags, which in
// turn controls how queries map to pool connections. Distinct tags is
// the only configuration that exercises pool fan-out — empty and
// same-tag serialise on one connection by design and exist as the
// lower-bound control for the speedup ratio.
void RunBurstPoolOn(benchmark::State& bstate,
                    const std::vector<std::string>& queries,
                    TagStrategy tag_strategy) {
  Rpc rpc;
  if (!LoadTraceInto(&rpc, bstate)) {
    return;
  }
  const size_t kN = queries.size();
  TaskQueue task_queue;
  std::vector<uint8_t> wire_bytes;
  std::mutex wire_mu;
  rpc.SetRpcResponseFunction([&](const void* data, uint32_t len) {
    std::lock_guard<std::mutex> g(wire_mu);
    auto* p = static_cast<const uint8_t*>(data);
    wire_bytes.insert(wire_bytes.end(), p, p + len);
  });
  rpc.SetResponseDispatcher(
      [&](std::function<void()> task) { task_queue.Post(std::move(task)); });

  auto await_n = [&](size_t n) {
    return task_queue.DrainUntil(
        [&] {
          std::lock_guard<std::mutex> g(wire_mu);
          return CountQueriesCompleted(wire_bytes) >= n;
        },
        std::chrono::seconds(60));
  };

  // Pre-warm: pull the lazy ThreadPool / Connection minting out of the
  // measured loop.
  {
    auto msg = EncodeStreamingQueryRpcMessage(0, "SELECT count(*) FROM slice");
    rpc.OnRpcRequest(msg.data(), msg.size());
    await_n(1);
    std::lock_guard<std::mutex> g(wire_mu);
    wire_bytes.clear();
  }

  int64_t seq = 1000;
  int64_t total_wall_ns = 0;
  int64_t total_sql_exec_ns = 0;
  int64_t total_dispatcher_ns = 0;
  for (auto _ : bstate) {
    {
      std::lock_guard<std::mutex> g(wire_mu);
      wire_bytes.clear();
    }
    rpc.reset_phase_timers_for_testing();
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kN; ++i) {
      auto msg = EncodeStreamingQueryRpcMessage(seq++, queries[i],
                                                TagFor(tag_strategy, i));
      rpc.OnRpcRequest(msg.data(), msg.size());
    }
    if (!await_n(kN)) {
      bstate.SkipWithError("Pool-on: drain timed out before kN completions");
      return;
    }
    auto t1 = std::chrono::steady_clock::now();
    total_wall_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    total_sql_exec_ns += rpc.sql_exec_ns_for_testing();
    total_dispatcher_ns += rpc.dispatcher_ns_for_testing();
    bstate.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
  }
  using Ctr = benchmark::Counter;
  bstate.counters["queries"] = Ctr(static_cast<double>(kN), Ctr::kIsIterationInvariant);
  bstate.counters["workers_used"] = Ctr(
      static_cast<double>(rpc.pool_workers_used_for_testing()),
      Ctr::kIsIterationInvariant);
  bstate.counters["distinct_connections"] = Ctr(
      static_cast<double>(rpc.pool_distinct_connections_for_testing()),
      Ctr::kIsIterationInvariant);
  bstate.counters["hardware_concurrency"] = Ctr(
      static_cast<double>(std::thread::hardware_concurrency()),
      Ctr::kIsIterationInvariant);
  // Diagnostic counters: how busy was the worker pool vs the single-
  // threaded transport dispatcher? Ideal speedup needs
  // worker_parallelism ≈ #pool_workers AND dispatcher_fraction << 1.
  if (total_wall_ns > 0) {
    bstate.counters["worker_parallelism"] =
        static_cast<double>(total_sql_exec_ns) /
        static_cast<double>(total_wall_ns);
    bstate.counters["dispatcher_fraction"] =
        static_cast<double>(total_dispatcher_ns) /
        static_cast<double>(total_wall_ns);
  }
}

// Pool OFF: no dispatcher. `OnRpcRequest` runs the streaming query
// inline on the calling thread (legacy synchronous path). All N
// queries run sequentially. Reports per-query times so we can compute
// `parallel_ceiling = sum / max` — the upper bound on the speedup
// the pool-on path could ever achieve on this workload.
void RunBurstPoolOff(benchmark::State& bstate,
                     const std::vector<std::string>& queries) {
  Rpc rpc;
  if (!LoadTraceInto(&rpc, bstate)) {
    return;
  }
  const size_t kN = queries.size();
  std::vector<uint8_t> wire_bytes;
  rpc.SetRpcResponseFunction([&](const void* data, uint32_t len) {
    auto* p = static_cast<const uint8_t*>(data);
    wire_bytes.insert(wire_bytes.end(), p, p + len);
  });

  // Pre-warm.
  {
    auto msg = EncodeStreamingQueryRpcMessage(0, "SELECT count(*) FROM slice");
    rpc.OnRpcRequest(msg.data(), msg.size());
    wire_bytes.clear();
  }

  int64_t seq = 1000;
  std::vector<int64_t> per_query_ns(kN, 0);
  int64_t iters = 0;
  for (auto _ : bstate) {
    wire_bytes.clear();
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kN; ++i) {
      auto q0 = std::chrono::steady_clock::now();
      auto msg = EncodeStreamingQueryRpcMessage(seq++, queries[i]);
      rpc.OnRpcRequest(msg.data(), msg.size());
      auto q1 = std::chrono::steady_clock::now();
      per_query_ns[i] +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(q1 - q0).count();
    }
    auto t1 = std::chrono::steady_clock::now();
    if (CountQueriesCompleted(wire_bytes) < kN) {
      bstate.SkipWithError("Pool-off: incomplete burst");
      return;
    }
    bstate.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
    iters++;
  }
  if (iters > 0) {
    int64_t max_ns = 0, sum_ns = 0;
    for (size_t i = 0; i < kN; ++i) {
      int64_t avg = per_query_ns[i] / iters;
      sum_ns += avg;
      max_ns = std::max(max_ns, avg);
      bstate.counters["q" + std::to_string(i) + "_us"] =
          static_cast<double>(avg) / 1e3;
    }
    bstate.counters["max_q_us"] = static_cast<double>(max_ns) / 1e3;
    bstate.counters["sum_q_us"] = static_cast<double>(sum_ns) / 1e3;
    bstate.counters["parallel_ceiling"] =
        max_ns > 0 ? static_cast<double>(sum_ns) / static_cast<double>(max_ns)
                   : 0.0;
  }
  using Ctr = benchmark::Counter;
  bstate.counters["queries"] = Ctr(static_cast<double>(kN), Ctr::kIsIterationInvariant);
  bstate.counters["hardware_concurrency"] = Ctr(
      static_cast<double>(std::thread::hardware_concurrency()),
      Ctr::kIsIterationInvariant);
}

#define BURST_POOL_OFF(name, queries)                       \
  static void BM_RpcBurst_##name##_PoolOff(                 \
      benchmark::State& bstate) {                           \
    RunBurstPoolOff(bstate, queries());                     \
  }                                                         \
  BENCHMARK(BM_RpcBurst_##name##_PoolOff)                   \
      ->UseManualTime()                                     \
      ->Unit(benchmark::kMillisecond)                       \
      ->MinTime(2.0)

#define BURST_POOL_ON(name, queries, strategy)              \
  static void BM_RpcBurst_##name##_PoolOn_##strategy(       \
      benchmark::State& bstate) {                           \
    RunBurstPoolOn(bstate, queries(), TagStrategy::k##strategy); \
  }                                                         \
  BENCHMARK(BM_RpcBurst_##name##_PoolOn_##strategy)         \
      ->UseManualTime()                                     \
      ->Unit(benchmark::kMillisecond)                       \
      ->MinTime(2.0)

BURST_POOL_OFF(Workload, WorkloadQueries);
BURST_POOL_ON(Workload, WorkloadQueries, Empty);
BURST_POOL_ON(Workload, WorkloadQueries, SameTag);
BURST_POOL_ON(Workload, WorkloadQueries, DistinctTags);
BURST_POOL_OFF(Balanced, BalancedWorkloadQueries);
BURST_POOL_ON(Balanced, BalancedWorkloadQueries, DistinctTags);
BURST_POOL_OFF(CpuOnly, CpuOnlyQueries);
BURST_POOL_ON(CpuOnly, CpuOnlyQueries, Empty);
BURST_POOL_ON(CpuOnly, CpuOnlyQueries, SameTag);
BURST_POOL_ON(CpuOnly, CpuOnlyQueries, DistinctTags);

#undef BURST_POOL_OFF
#undef BURST_POOL_ON

}  // namespace
}  // namespace perfetto::trace_processor
