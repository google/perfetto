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

#ifndef SRC_TRACE_PROCESSOR_RPC_RPC_H_
#define SRC_TRACE_PROCESSOR_RPC_RPC_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/threading/thread_pool.h"
#include "perfetto/ext/protozero/proto_ring_buffer.h"
#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/summarizer.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto {

namespace protos::pbzero {
class ComputeMetricResult;
class DisableAndReadMetatraceResult;
}  // namespace protos::pbzero

namespace trace_processor {

class Iterator;

// This class handles the binary {,un}marshalling for the Trace Processor RPC
// API (see protos/perfetto/trace_processor/trace_processor.proto).
// This is to deal with cases where the client of the trace processor is not
// some in-process C++ code but a remote process:
// There are two use cases of this:
//   1. The JS<>WASM interop for the web-based UI.
//   2. The HTTP RPC mode of trace_processor_shell that allows the UI to talk
//      to a native trace processor instead of the bundled WASM one.
// This class has (a subset of) the same methods of the public TraceProcessor
// interface, but the methods just take and return proto-encoded binary buffers.
// This class does NOT define how the transport works (e.g. HTTP vs WASM interop
// calls), it just deals with {,un}marshalling.
// This class internally creates and owns a TraceProcessor instance, which
// lifetime is tied to the lifetime of the Rpc instance.
class Rpc {
 public:
  // The unique_ptr argument is optional. If non-null it will adopt the passed
  // instance and allow to directly query that. If null, a new instanace will be
  // created internally by calling Parse().
  explicit Rpc(std::unique_ptr<TraceProcessor>,
               bool has_preloaded_eof,
               Config default_config,
               std::function<void(TraceProcessor*)> on_trace_processor_created);
  Rpc();
  ~Rpc();

  // 1. TraceProcessor byte-pipe RPC interface.
  // This is a bidirectional channel with a remote TraceProcessor instance. All
  // it needs is a byte-oriented pipe (e.g., a TCP socket, a pipe(2) between two
  // processes or a postmessage channel in the JS+Wasm case). The messages
  // exchanged on these pipes are TraceProcessorRpc protos (defined in
  // trace_processor.proto). This has been introduced in Perfetto v15.

  // Pushes data received by the RPC channel into the parser. Inbound messages
  // are tokenized and turned into TraceProcessor method invocations. |data|
  // does not need to be a whole TraceProcessorRpc message. It can be a portion
  // of it or a union of >1 messages.
  // Responses are sent throught the RpcResponseFunction (below).
  void OnRpcRequest(const void* data, size_t len);

  // The size argument is a uint32_t and not size_t to avoid ABI mismatches
  // with Wasm, where size_t = uint32_t.
  // (nullptr, 0) has the semantic of "close the channel" and is issued when an
  // unrecoverable wire-protocol framing error is detected.
  using RpcResponseFunction =
      std::function<void(const void* /*data*/, uint32_t /*len*/)>;
  void SetRpcResponseFunction(RpcResponseFunction f) {
    rpc_response_fn_ = std::move(f);
  }

  // Optional. When set, `TPM_QUERY_STREAMING` requests received post-EOF
  // are dispatched to the worker pool: a worker thread acquires a
  // `Connection`, executes the query, materialises the chunks, and then
  // hands them back to the transport thread by invoking the dispatcher
  // with a "send all chunks for this query" closure. The dispatcher's
  // job is to schedule that closure on whichever thread owns
  // `rpc_response_fn_` (typically the http task runner). The closure
  // calls `rpc_response_fn_` directly and is responsible for assigning
  // the trailing `tx_seq_id_`s — which it does in dispatcher (== task
  // runner) order so per-query chunks stay contiguous and the
  // request->response ordering observed by the UI is preserved across
  // concurrent streaming queries (workers run sqlite in parallel; the
  // `Send` step is serialised on the transport thread).
  // Calling `OnRpcRequest` from a thread other than the dispatcher's
  // target thread is unsupported once a dispatcher is set.
  using ResponseDispatcher = std::function<void(std::function<void()>)>;
  void SetResponseDispatcher(ResponseDispatcher d) {
    response_dispatcher_ = std::move(d);
  }

  // 2. TraceProcessor legacy RPC endpoints.
  // The methods below are exposed for the old RPC interfaces, where each RPC
  // implementation deals with the method demuxing: (i) wasm_bridge.cc has one
  // exported C function per method (going away soon); (ii) httpd.cc has one
  // REST endpoint per method. Over time this turned out to have too much
  // duplicated boilerplate and we moved to the byte-pipe model above.
  // We still keep these endpoints around, because httpd.cc still  exposes the
  // individual REST endpoints to legacy clients (TP's Python API). The
  // mainteinance cost of those is very low. Both the new byte-pipe and the
  // old endpoints run exactly the same code. The {de,}serialization format is
  // the same, the only difference is only who does the method demuxing.
  // The methods of this class are mirrors (modulo {un,}marshalling of args) of
  // the corresponding names in trace_processor.h . See that header for docs.

  base::Status Parse(const uint8_t*, size_t);
  base::Status NotifyEndOfFile();
  std::string GetCurrentTraceName();
  std::vector<uint8_t> ComputeMetric(const uint8_t*, size_t);
  std::vector<uint8_t> ComputeTraceSummary(const uint8_t*, size_t);
  void EnableMetatrace(const uint8_t*, size_t);
  std::vector<uint8_t> DisableAndReadMetatrace();
  std::vector<uint8_t> GetStatus();

  // Creates a new RPC session by deleting all tables and views that have been
  // created (by the UI or user) after the trace was loaded; built-in
  // tables/view created by the ingestion process are preserved.
  void RestoreInitialTables();

  // Runs a query and returns results in batch. Each batch is a proto-encoded
  // TraceProcessor.QueryResult message and contains a variable number of rows.
  // The callbacks are called inline, so the whole callstack looks as follows:
  // Query(..., callback)
  //   callback(..., has_more=true)
  //   ...
  //   callback(..., has_more=false)
  //   (Query() returns at this point).
  using QueryResultBatchCallback = std::function<
      void(const uint8_t* /*buf*/, size_t /*len*/, bool /*has_more*/)>;
  void Query(const uint8_t*, size_t, const QueryResultBatchCallback&);

  TraceProcessor* trace_processor() const { return trace_processor_.get(); }

  // Test-only counters exposing pool behaviour. Not part of the wire API.
  uint32_t pool_workers_used_for_testing() const {
    std::lock_guard<std::mutex> g(pool_mu_);
    return static_cast<uint32_t>(distinct_worker_thread_ids_.size());
  }
  uint32_t pool_distinct_connections_for_testing() const {
    std::lock_guard<std::mutex> g(pool_mu_);
    return distinct_connections_minted_;
  }
  uint32_t streaming_async_dispatches_for_testing() const {
    std::lock_guard<std::mutex> g(pool_mu_);
    return streaming_async_dispatches_;
  }
  size_t tag_slots_size_for_testing() const {
    std::lock_guard<std::mutex> g(tag_mu_);
    return tag_slots_.size();
  }
  size_t affinity_size_for_testing() const {
    std::lock_guard<std::mutex> g(pool_mu_);
    return tag_to_conn_.size();
  }
  // Returns the conn id currently affined to `tag`, or -1 if none.
  int64_t affinity_for_tag_for_testing(const std::string& tag) const {
    std::lock_guard<std::mutex> g(pool_mu_);
    auto* e = tag_to_conn_.Find(tag);
    return e ? static_cast<int64_t>(e->conn_id) : -1;
  }
  // Per-phase wall-time accumulators (ns). Workers add to
  // `sql_exec` for the inclusive Acquire→Release span (the actual
  // ExecuteQuery + Serialize work) and the transport thread adds to
  // `dispatcher` for the inclusive drain-closure span. Comparing
  // their sums against the burst's wall-time is how the benchmark
  // diagnoses "is the worker pool bottlenecked or is dispatch?":
  //   worker_parallelism = sql_exec_ns / wall_ns   (caps at #workers)
  //   dispatcher_fraction = dispatcher_ns / wall_ns (≈1.0 means the
  //                                                  transport
  //                                                  thread is busy
  //                                                  the whole time)
  void reset_phase_timers_for_testing() {
    sql_exec_ns_.store(0, std::memory_order_relaxed);
    dispatcher_ns_.store(0, std::memory_order_relaxed);
  }
  int64_t sql_exec_ns_for_testing() const {
    return sql_exec_ns_.load(std::memory_order_relaxed);
  }
  int64_t dispatcher_ns_for_testing() const {
    return dispatcher_ns_.load(std::memory_order_relaxed);
  }

 private:
  void ParseRpcRequest(const uint8_t*, size_t);
  void ResetTraceProcessor(const uint8_t*, size_t);
  base::Status RegisterSqlPackage(protozero::ConstBytes);
  void ResetTraceProcessorInternal(const Config&);
  void MaybePrintProgress();
  Iterator QueryInternal(const uint8_t*, size_t);
  void ComputeMetricInternal(const uint8_t*,
                             size_t,
                             protos::pbzero::ComputeMetricResult*);
  void ComputeTraceSummaryInternal(const uint8_t*,
                                   size_t,
                                   protos::pbzero::TraceSummaryResult*);
  void DisableAndReadMetatraceInternal(
      protos::pbzero::DisableAndReadMetatraceResult*);

  // Worker-pool plumbing for `Query` fan-out. The pool is only used after
  // `NotifyEndOfFile` (strict-v1: secondary connections are illegal pre-EOF).
  // A `PooledConnection` is the connection plus its mint-time id; the id
  // is the key of the soft tag-affinity map so a tag's queries can prefer
  // the same underlying connection when it's free.
  struct PooledConnection {
    uint32_t id = 0;
    std::unique_ptr<TraceProcessor::Connection> conn;
    PooledConnection() = default;
    PooledConnection(uint32_t i,
                     std::unique_ptr<TraceProcessor::Connection> c)
        : id(i), conn(std::move(c)) {}
    PooledConnection(PooledConnection&&) noexcept = default;
    PooledConnection& operator=(PooledConnection&&) noexcept = default;
  };
  PooledConnection AcquireConnectionForQuery(const std::string& tag);
  void ReleaseConnectionToPool(PooledConnection pooled);
  void RunQueryOnPoolWorker(std::string sql,
                            base::TimeNanos t_start,
                            const QueryResultBatchCallback& result_callback);
  // Mints worker-pool threads + the very first connection on the
  // caller's thread, idempotently. Shared between the sync `Rpc::Query`
  // path and the async streaming dispatch.
  void EnsureWorkerPoolPrimed();
  // Lazy-growth helper: if all pooled connections are in use and we
  // haven't hit hardware_concurrency, mint one more. Called on the
  // writer thread before dispatching to keep CreateConnection
  // single-producer.
  void MaybeGrowConnectionPool();
  // Async streaming dispatch entry point used from `ParseRpcRequest` when
  // `response_dispatcher_` is set and the trace has hit EOF. Posts a
  // worker-pool task that materialises chunks then dispatches the "send
  // chunks" closure back on the transport thread; ordering of concurrent
  // streaming queries is preserved by the in-order `streaming_send_*`
  // queue (see private members below).
  void DispatchStreamingQueryAsync(std::string sql,
                                   base::TimeNanos t_start,
                                   int req_type,
                                   std::string tag);
  // Worker-pool-task body shared between the initial dispatch and
  // same-tag dequeue dispatch. Takes a snapshot of `response_fn_`,
  // already-claimed slot, sql, and tag.
  struct PendingTaggedQuery;
  void PostTaggedQueryToWorker(std::string tag, PendingTaggedQuery q);

  Config default_config_;
  std::function<void(TraceProcessor*)> on_trace_processor_created_;

  Config current_config_;
  std::unique_ptr<TraceProcessor> trace_processor_;
  RpcResponseFunction rpc_response_fn_;
  protozero::ProtoRingBuffer rxbuf_;
  int64_t tx_seq_id_ = 0;
  int64_t rx_seq_id_ = 0;
  bool eof_ = false;
  int64_t t_parse_started_ = 0;
  size_t bytes_last_progress_ = 0;
  size_t bytes_parsed_ = 0;

  // Manages Summarizer instances keyed by caller-provided ID.
  base::FlatHashMap<std::string, std::unique_ptr<Summarizer>> summarizers_;

  // Connection pool + worker pool. The worker pool is sized to
  // `min(hardware_concurrency, 8)` (or 1 if `hardware_concurrency` is 0)
  // and is created lazily on first post-EOF `Query`. The connection pool
  // is unbounded; idle connections live on `pool_free_`.
  //
  // TP-level mutating RPCs (RegisterSqlPackage, RestoreInitialTables,
  // ResetTraceProcessor, etc.) are NOT drain-coordinated by this layer;
  // the underlying `TraceProcessor` enforces its own
  // `non_default_connection_count_ == 0` precondition with a
  // PERFETTO_CHECK and will crash if a caller invokes a mutation while
  // pooled connections are alive. Folding the precondition into TP
  // itself (so callers don't need to know) is a pending refactor.
  mutable std::mutex pool_mu_;
  std::condition_variable pool_cv_;
  std::vector<PooledConnection> pool_free_;
  uint32_t pool_in_use_ = 0;
  uint32_t distinct_connections_minted_ = 0;
  // Soft tag-affinity: tag -> conn id (last connection a given tag
  // ran on). On Acquire, prefer the affined connection if it's in the
  // free list; otherwise fall back to any free connection and update
  // the affinity. Bounded by `kMaxAffinityEntries` via LRU eviction
  // so the map can't grow without bound when tags are short-lived.
  // Stale affinities (the affined conn no longer in `pool_free_`,
  // either in use or evicted) are silently ignored — the design is
  // intentionally cheap-best-effort.
  static constexpr size_t kMaxAffinityEntries = 64;
  struct AffinityEntry {
    uint32_t conn_id;
    std::list<std::string>::iterator lru_iter;
  };
  std::list<std::string> affinity_lru_;  // MRU at front
  base::FlatHashMap<std::string, AffinityEntry> tag_to_conn_;
  std::unordered_set<std::thread::id> distinct_worker_thread_ids_;
  // Serialises calls to `TraceProcessor::CreateConnection`. Any thread
  // can post a Query, but only one of them at a time may mint
  // connections (and only ever from outside the worker pool's threads),
  // because `CreateConnection` is the single writer of
  // `StringPool::should_acquire_mutex_`. Concurrent `CreateConnection`
  // calls from multiple Query callers would otherwise race.
  std::mutex pool_mint_mu_;
  std::unique_ptr<base::ThreadPool> worker_pool_;

  // Async streaming dispatch state. `response_dispatcher_` is the
  // transport-supplied PostTask; the streaming path uses it to send
  // chunks back on the transport thread. The two `streaming_send_*`
  // counters serialise concurrent streaming queries' *sends* so the UI
  // pendingQueries[0]-FIFO invariant holds: each dispatched query
  // increments `streaming_send_next_seq_` to claim a slot; a worker
  // signals completion by inserting into `streaming_send_ready_` which
  // is drained in slot order on the transport thread.
  ResponseDispatcher response_dispatcher_;
  uint64_t streaming_send_next_seq_ = 0;       // claimed under pool_mu_
  uint64_t streaming_send_drain_cursor_ = 0;   // accessed only on dispatcher
  // Worker-deposited results, keyed by claimed slot. Drained in slot
  // order on the transport thread. The chunks are pre-serialised
  // QueryResult bytes; the transport-thread closure wraps each one in a
  // RpcProto with a freshly-assigned `tx_seq_id_` and ships it via the
  // `response_fn` snapshot taken at dispatch time. Snapshotting per
  // dispatch lets each query route back to its originating transport
  // even if `rpc_response_fn_` has since been overwritten by a later
  // OnRpcRequest from a different connection.
  struct StreamingResult {
    int req_type;
    std::vector<std::vector<uint8_t>> chunks;
    RpcResponseFunction response_fn;
  };
  base::FlatHashMap<uint64_t, StreamingResult> streaming_send_ready_;
  uint32_t streaming_async_dispatches_ = 0;

  // Tag-affine dispatch state. Each streaming query carries a
  // `QueryArgs.tag` (forwarded by the UI's `EngineProxy`); same-tag
  // queries serialise so plugin/track sessions get connection-affinity
  // benefits (warm per-conn page cache, prepared-statement reuse) at
  // the cost of intra-tag concurrency. Different-tag queries fan out
  // across the pool. A tag's slot stays in the map only while a query
  // is in flight or queued; idle tags are erased to bound the map.
  struct PendingTaggedQuery {
    std::string sql;
    base::TimeNanos t_start;
    int req_type;
    RpcResponseFunction response_fn;
    uint64_t slot;
  };
  struct TagSlot {
    bool in_flight = false;
    std::deque<PendingTaggedQuery> queue;
  };
  mutable std::mutex tag_mu_;
  base::FlatHashMap<std::string, TagSlot> tag_slots_;

  // See `reset_phase_timers_for_testing` for usage.
  std::atomic<int64_t> sql_exec_ns_{0};
  std::atomic<int64_t> dispatcher_ns_{0};
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_RPC_RPC_H_
