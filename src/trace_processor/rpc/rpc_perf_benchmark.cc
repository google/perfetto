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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "perfetto/base/logging.h"
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
// over the busiest tables (slice, thread_state, sched, counter, args).
// Each query touches enough rows that the per-query cost dominates
// dispatch overhead — count(*) probes alone run sub-millisecond on
// the fixture trace, well below the worker pool's PostTask + drain
// cost, and so don't show the pool's parallelism win. Group-bys with
// modest result sets are a closer match to what the UI's track-
// summarisation queries look like at trace-load time.
//
// All queries are deliberately independent (no inter-query state) so
// the pool can fan them out trivially.
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

// Same shape as `WorkloadQueries` but every query has approximately
// the same cost: 10 copies of `SELECT name, count(*), sum(dur) FROM
// slice GROUP BY name ORDER BY s DESC LIMIT 100`. This factors
// Amdahl's-law variance out of the speedup measurement: under the
// uneven `WorkloadQueries`, wall-time is bounded by the slowest
// single query (sum/max ≈ 1.7), and so the pool can never beat that
// regardless of fan-out. With balanced costs, sum/max ≈ N and the
// achieved speedup is a clean read of how well the dispatch path
// fans out across worker connections. Distinct GROUP BY columns give
// each query its own work without sharing per-table sort buffers.
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

// Encodes a `TraceProcessorRpc` framing-prefixed wire message that
// asks the server to run a streaming query for the given SQL. Mirrors
// `EncodeStreamingQueryRpcMessage` in `rpc_unittest.cc`. If `tag` is
// non-empty it is set on the QueryArgs, opting the request into the
// tag-affine dispatch path (distinct tags fan out across connections,
// same-tag queries serialise on one connection).
std::vector<uint8_t> EncodeStreamingQueryRpcMessage(int64_t seq,
                                                    const std::string& sql,
                                                    const std::string& tag = {}) {
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

// Counts the number of TPM_QUERY_STREAMING responses where
// `is_last_batch=true`, i.e. the number of *queries* that completed
// (not the number of chunks).
size_t CountQueriesCompleted(const std::vector<uint8_t>& wire_bytes) {
  protos::pbzero::TraceProcessorRpcStream::Decoder stream(wire_bytes.data(),
                                                          wire_bytes.size());
  size_t completed = 0;
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

// A trivial multi-producer single-consumer task queue used to stand
// in for `MaybeLockFreeTaskRunner`. Workers post completion closures
// here; the driver thread drains them.
class TaskQueue {
 public:
  void Post(std::function<void()> task) {
    std::lock_guard<std::mutex> g(mu_);
    q_.push_back(std::move(task));
    cv_.notify_all();
  }

  // Drains tasks until `predicate()` returns true OR the deadline is
  // reached. Returns true if the predicate was satisfied. Tasks that
  // arrive while we hold the lock are processed in FIFO order.
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

// Loads the fixture trace into a freshly-constructed `Rpc` instance
// and returns true on success. On failure, `bstate.SkipWithError` is
// called so the benchmark report flags the missing fixture.
bool LoadTraceInto(Rpc* rpc, benchmark::State& bstate) {
  std::string contents;
  if (!base::ReadFile(kTraceFixture, &contents)) {
    bstate.SkipWithError(
        "Test data missing. Please ensure "
        "test/data/android_postboot_unlock.pftrace exists.");
    return false;
  }
  base::Status s = rpc->Parse(reinterpret_cast<const uint8_t*>(contents.data()),
                              contents.size());
  if (!s.ok()) {
    bstate.SkipWithError(
        ("Rpc::Parse failed: " + s.message()).c_str());
    return false;
  }
  s = rpc->NotifyEndOfFile();
  if (!s.ok()) {
    bstate.SkipWithError(
        ("Rpc::NotifyEndOfFile failed: " + s.message()).c_str());
    return false;
  }
  return true;
}

// How a burst's queries should be tagged. `kEmpty` and `kSameTag`
// both route to a single connection (same logical session); they
// differ only in whether the request opts into the tag-affine
// dispatch path explicitly. `kDistinctTags` gives each query its own
// tag so different-tag requests fan out across the pool.
enum class TagStrategy { kEmpty, kSameTag, kDistinctTags };

std::string TagFor(TagStrategy s, size_t i) {
  switch (s) {
    case TagStrategy::kEmpty: return {};
    case TagStrategy::kSameTag: return "shared";
    case TagStrategy::kDistinctTags: return "tag_" + std::to_string(i);
  }
  return {};
}

// Pool ON: queries dispatch async through a wired-up dispatcher.
// Mirrors what `httpd.cc` does at construction.
//
// Each iteration: fire all N queries via OnRpcRequest, then drain the
// task queue until N is_last_batch markers have been observed. Wall
// time = elapsed from first dispatch to last drained completion.
//
// `tag_strategy` controls how the burst maps to RPC tags, which in
// turn controls how queries map to pool connections. Distinct tags is
// the only configuration that exercises pool fan-out — the empty and
// same-tag configurations serialise on one connection by design and
// exist as the lower-bound control for the speedup ratio.
void RunStreamingBurstPoolOn(benchmark::State& bstate,
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
  rpc.SetResponseDispatcher([&](std::function<void()> task) {
    task_queue.Post(std::move(task));
  });

  // Pre-warm: one query so the worker pool + connection pool are
  // primed. This pulls the lazy ThreadPool / Connection minting cost
  // out of the measured loop.
  {
    auto msg = EncodeStreamingQueryRpcMessage(/*seq=*/0,
                                              "SELECT count(*) FROM slice");
    rpc.OnRpcRequest(msg.data(), msg.size());
    task_queue.DrainUntil(
        [&]() {
          std::lock_guard<std::mutex> g(wire_mu);
          return CountQueriesCompleted(wire_bytes) >= 1;
        },
        std::chrono::seconds(60));
    std::lock_guard<std::mutex> g(wire_mu);
    wire_bytes.clear();
  }

  int64_t seq = 1000;
  // Per-iteration phase totals; convert to averages at end.
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
    bool done = task_queue.DrainUntil(
        [&]() {
          std::lock_guard<std::mutex> g(wire_mu);
          return CountQueriesCompleted(wire_bytes) >= kN;
        },
        std::chrono::seconds(60));
    auto t1 = std::chrono::steady_clock::now();
    if (!done) {
      bstate.SkipWithError("Pool-on: drain timed out before kN completions");
      return;
    }
    const auto wall_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    total_wall_ns += wall_ns;
    total_sql_exec_ns += rpc.sql_exec_ns_for_testing();
    total_dispatcher_ns += rpc.dispatcher_ns_for_testing();
    bstate.SetIterationTime(
        std::chrono::duration<double>(t1 - t0).count());
  }
  bstate.counters["queries"] =
      benchmark::Counter(static_cast<double>(kN),
                         benchmark::Counter::kIsIterationInvariant);
  bstate.counters["workers_used"] = benchmark::Counter(
      static_cast<double>(rpc.pool_workers_used_for_testing()),
      benchmark::Counter::kIsIterationInvariant);
  bstate.counters["distinct_connections"] = benchmark::Counter(
      static_cast<double>(rpc.pool_distinct_connections_for_testing()),
      benchmark::Counter::kIsIterationInvariant);
  bstate.counters["hardware_concurrency"] = benchmark::Counter(
      static_cast<double>(std::thread::hardware_concurrency()),
      benchmark::Counter::kIsIterationInvariant);
  // Diagnostic counters: how busy was the worker pool vs the
  // single-threaded transport dispatcher? Ideal speedup needs
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

void BM_RpcStreamingQueryBurst_PoolOn_Untagged(benchmark::State& bstate) {
  RunStreamingBurstPoolOn(bstate, WorkloadQueries(), TagStrategy::kEmpty);
}
void BM_RpcStreamingQueryBurst_PoolOn_SameTag(benchmark::State& bstate) {
  RunStreamingBurstPoolOn(bstate, WorkloadQueries(), TagStrategy::kSameTag);
}
void BM_RpcStreamingQueryBurst_PoolOn_DistinctTags(benchmark::State& bstate) {
  RunStreamingBurstPoolOn(bstate, WorkloadQueries(),
                          TagStrategy::kDistinctTags);
}
void BM_RpcStreamingQueryBurst_Balanced_PoolOn_DistinctTags(
    benchmark::State& bstate) {
  RunStreamingBurstPoolOn(bstate, BalancedWorkloadQueries(),
                          TagStrategy::kDistinctTags);
}

// Pool OFF: no dispatcher. `OnRpcRequest` runs the streaming query
// inline on the calling thread (the legacy synchronous path). All N
// queries run sequentially. Reports per-query times so we can see
// `parallel_ceiling = sum / max` — the upper bound on the speedup
// the pool-on path could ever achieve on this workload.
void RunStreamingBurstPoolOff(benchmark::State& bstate,
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
  // No SetResponseDispatcher -> sync inline path.

  // Pre-warm: same as pool-on, to amortise any first-query JIT/cache.
  {
    auto msg = EncodeStreamingQueryRpcMessage(/*seq=*/0,
                                              "SELECT count(*) FROM slice");
    rpc.OnRpcRequest(msg.data(), msg.size());
    wire_bytes.clear();
  }

  int64_t seq = 1000;
  // Per-query wall times (ns) summed across iterations. Reported as
  // diagnostic counters so we can see if pool-on speedup is bounded
  // by the slowest single query (Amdahl's law: wall_pool_on can't
  // drop below max(per_query_time)).
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
    size_t completed = CountQueriesCompleted(wire_bytes);
    if (completed < kN) {
      bstate.SkipWithError(
          ("Pool-off: only " + std::to_string(completed) + "/" +
           std::to_string(kN) + " queries completed").c_str());
      return;
    }
    bstate.SetIterationTime(
        std::chrono::duration<double>(t1 - t0).count());
    iters++;
  }
  if (iters > 0) {
    int64_t max_avg_ns = 0;
    int64_t sum_avg_ns = 0;
    for (size_t i = 0; i < kN; ++i) {
      int64_t avg = per_query_ns[i] / iters;
      sum_avg_ns += avg;
      if (avg > max_avg_ns) max_avg_ns = avg;
      bstate.counters["q" + std::to_string(i) + "_us"] = static_cast<double>(avg) / 1e3;
    }
    bstate.counters["max_q_us"] = static_cast<double>(max_avg_ns) / 1e3;
    bstate.counters["sum_q_us"] = static_cast<double>(sum_avg_ns) / 1e3;
    // Parallelism ceiling under perfect 10-way fan-out: sum / max.
    bstate.counters["parallel_ceiling"] =
        max_avg_ns > 0
            ? static_cast<double>(sum_avg_ns) / static_cast<double>(max_avg_ns)
            : 0.0;
  }
  bstate.counters["queries"] =
      benchmark::Counter(static_cast<double>(kN),
                         benchmark::Counter::kIsIterationInvariant);
  bstate.counters["hardware_concurrency"] = benchmark::Counter(
      static_cast<double>(std::thread::hardware_concurrency()),
      benchmark::Counter::kIsIterationInvariant);
}

void BM_RpcStreamingQueryBurst_PoolOff(benchmark::State& bstate) {
  RunStreamingBurstPoolOff(bstate, WorkloadQueries());
}
void BM_RpcStreamingQueryBurst_Balanced_PoolOff(benchmark::State& bstate) {
  RunStreamingBurstPoolOff(bstate, BalancedWorkloadQueries());
}

// Parallelism diagnostic: a workload that touches no shared trace
// tables. Uses a recursive CTE to do pure CPU work on each connection
// independently. The headline reading is "what's the dispatch ceiling
// when SQL execution itself doesn't touch any shared state".
const std::vector<std::string>& CpuOnlyQueries() {
  static const std::vector<std::string> kQueries = {
      // 50K-row recursive CTE summed with a hash-style transform.
      // Each query is independent and touches no trace tables.
      "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
      "WHERE x<50000) SELECT sum(x*x*x) FROM c",
      "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
      "WHERE x<50000) SELECT sum(x*x*x) FROM c",
      "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
      "WHERE x<50000) SELECT sum(x*x*x) FROM c",
      "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
      "WHERE x<50000) SELECT sum(x*x*x) FROM c",
      "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
      "WHERE x<50000) SELECT sum(x*x*x) FROM c",
      "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
      "WHERE x<50000) SELECT sum(x*x*x) FROM c",
      "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
      "WHERE x<50000) SELECT sum(x*x*x) FROM c",
      "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
      "WHERE x<50000) SELECT sum(x*x*x) FROM c",
  };
  return kQueries;
}

void BM_RpcCpuOnlyBurst_PoolOn_Untagged(benchmark::State& bstate) {
  RunStreamingBurstPoolOn(bstate, CpuOnlyQueries(), TagStrategy::kEmpty);
}
void BM_RpcCpuOnlyBurst_PoolOn_SameTag(benchmark::State& bstate) {
  RunStreamingBurstPoolOn(bstate, CpuOnlyQueries(), TagStrategy::kSameTag);
}
void BM_RpcCpuOnlyBurst_PoolOn_DistinctTags(benchmark::State& bstate) {
  RunStreamingBurstPoolOn(bstate, CpuOnlyQueries(),
                          TagStrategy::kDistinctTags);
}

void BM_RpcCpuOnlyBurst_PoolOff(benchmark::State& bstate) {
  Rpc rpc;
  if (!LoadTraceInto(&rpc, bstate)) {
    return;
  }
  const auto& queries = CpuOnlyQueries();
  const size_t kN = queries.size();

  std::vector<uint8_t> wire_bytes;
  rpc.SetRpcResponseFunction([&](const void* data, uint32_t len) {
    auto* p = static_cast<const uint8_t*>(data);
    wire_bytes.insert(wire_bytes.end(), p, p + len);
  });
  // Pre-warm.
  {
    auto msg = EncodeStreamingQueryRpcMessage(0, queries[0]);
    rpc.OnRpcRequest(msg.data(), msg.size());
    wire_bytes.clear();
  }

  int64_t seq = 1000;
  for (auto _ : bstate) {
    wire_bytes.clear();
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kN; ++i) {
      auto msg = EncodeStreamingQueryRpcMessage(seq++, queries[i]);
      rpc.OnRpcRequest(msg.data(), msg.size());
    }
    auto t1 = std::chrono::steady_clock::now();
    if (CountQueriesCompleted(wire_bytes) < kN) {
      bstate.SkipWithError("CpuOnly pool-off: incomplete");
      return;
    }
    bstate.SetIterationTime(
        std::chrono::duration<double>(t1 - t0).count());
  }
  bstate.counters["queries"] =
      benchmark::Counter(static_cast<double>(kN),
                         benchmark::Counter::kIsIterationInvariant);
}

BENCHMARK(BM_RpcStreamingQueryBurst_PoolOff)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);
BENCHMARK(BM_RpcStreamingQueryBurst_PoolOn_Untagged)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);
BENCHMARK(BM_RpcStreamingQueryBurst_PoolOn_SameTag)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);
BENCHMARK(BM_RpcStreamingQueryBurst_PoolOn_DistinctTags)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);
BENCHMARK(BM_RpcStreamingQueryBurst_Balanced_PoolOff)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);
BENCHMARK(BM_RpcStreamingQueryBurst_Balanced_PoolOn_DistinctTags)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);
BENCHMARK(BM_RpcCpuOnlyBurst_PoolOff)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);
BENCHMARK(BM_RpcCpuOnlyBurst_PoolOn_Untagged)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);
BENCHMARK(BM_RpcCpuOnlyBurst_PoolOn_SameTag)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);
BENCHMARK(BM_RpcCpuOnlyBurst_PoolOn_DistinctTags)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);

}  // namespace
}  // namespace perfetto::trace_processor
