/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACING_SERVICE_TRACING_SERVICE_SESSION_H_
#define SRC_TRACING_SERVICE_TRACING_SERVICE_SESSION_H_

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/circular_queue.h"
#include "perfetto/ext/base/clock_snapshots.h"
#include "perfetto/ext/base/periodic_task.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/scoped_sched_boost.h"
#include "perfetto/ext/base/uuid.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_stats.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/trace_config.h"

#include "src/tracing/service/tracing_service_structs.h"

namespace protozero {
class MessageFilter;
}

namespace perfetto {

namespace base {
class TaskRunner;
}

namespace tracing_service {

class ConsumerEndpointImpl;

// Holds the state of a tracing session. A tracing session is uniquely bound
// a specific Consumer. Each Consumer can own one or more sessions.
struct TracingSession {
  enum State {
    DISABLED = 0,
    CONFIGURED,
    STARTED,
    DISABLING_WAITING_STOP_ACKS,
    CLONED_READ_ONLY,
  };

  TracingSession(TracingSessionID,
                 ConsumerEndpointImpl*,
                 const TraceConfig&,
                 base::TaskRunner*);
  TracingSession(TracingSession&&) = delete;
  TracingSession& operator=(TracingSession&&) = delete;

  size_t num_buffers() const { return buffers_index.size(); }

  uint32_t flush_timeout_ms() {
    uint32_t timeout_ms = config.flush_timeout_ms();
    return timeout_ms ? timeout_ms : kDefaultFlushTimeoutMs;
  }

  uint32_t data_source_stop_timeout_ms() {
    uint32_t timeout_ms = config.data_source_stop_timeout_ms();
    return timeout_ms ? timeout_ms : kDataSourceStopTimeoutMs;
  }

  PacketSequenceID GetPacketSequenceID(MachineID, ProducerID, WriterID);
  DataSourceInstance* GetDataSourceInstance(ProducerID, DataSourceInstanceID);
  bool AllDataSourceInstancesStarted();
  bool AllDataSourceInstancesStopped();

  // Checks whether |clone_uid| is allowed to clone the current tracing
  // session.
  bool IsCloneAllowed(uid_t clone_uid) const;

  // Records |src|'s current state into concurrent_session_events, dropping the
  // event once the buffer hits the limit. The cap bounds memory, as the buffer
  // is drained only on ReadBuffers().
  void AddConcurrentSessionEventWithLimit(const TracingSession& src);

  const TracingSessionID id;

  // The consumer that started the session.
  // Can be nullptr if the consumer detached from the session.
  ConsumerEndpointImpl* consumer_maybe_null;

  // Unix uid of the consumer. This is valid even after the consumer detaches
  // and does not change for the entire duration of the session. It is used to
  // prevent that a consumer re-attaches to a session from a different uid.
  uid_t const consumer_uid;

  // The list of triggers this session received while alive and the time they
  // were received at. This is used to insert 'fake' packets back to the
  // consumer so they can tell when some event happened. The order matches the
  // order they were received.
  std::vector<TriggerInfo> received_triggers;

  // The trace config provided by the Consumer when calling
  // EnableTracing(), plus any updates performed by ChangeTraceConfig.
  TraceConfig config;

  // List of data source instances that have been enabled on the various
  // producers for this tracing session.
  std::multimap<ProducerID, DataSourceInstance> data_source_instances;

  // For each Flush(N) request, keeps track of the set of producers for which
  // we are still awaiting a NotifyFlushComplete(N) ack.
  std::map<FlushRequestID, PendingFlush> pending_flushes;

  // For each Clone request, keeps track of the flushes acknowledgement that
  // we are still waiting for.
  std::map<PendingCloneID, PendingClone> pending_clones;

  PendingCloneID last_pending_clone_id_ = 0;

  // Maps a per-trace-session buffer index into the corresponding global
  // BufferID (shared namespace amongst all consumers). This vector has as
  // many entries as |config.buffers_size()|.
  std::vector<BufferID> buffers_index;

  std::map<std::tuple<MachineID, ProducerID, WriterID>, PacketSequenceID>
      packet_sequence_ids;
  PacketSequenceID last_packet_sequence_id = kServicePacketSequenceID;

  // Whether we should emit the trace stats next time we reach EOF while
  // performing ReadBuffers.
  bool should_emit_stats = false;

  // Whether we should emit the sync marker the next time ReadBuffers() is
  // called.
  bool should_emit_sync_marker = false;

  // Whether we put the initial packets (trace config, system info,
  // etc.) into the trace output yet.
  bool did_emit_initial_packets = false;

  // Whether we emitted clock offsets for relay clients yet.
  bool did_emit_remote_clock_sync_ = false;

  // Whether we emitted the ProtoVM instances.
  bool did_emit_protovm_instances_ = false;

  // The number of received triggers we've emitted into the trace output.
  size_t num_triggers_emitted_into_trace = 0;

  // Packets that failed validation of the TrustedPacket.
  uint64_t invalid_packets = 0;

  // Flush() stats. See comments in trace_stats.proto for more.
  uint64_t flushes_requested = 0;
  uint64_t flushes_succeeded = 0;
  uint64_t flushes_failed = 0;

  // Outcome of the final Flush() done by FlushAndDisableTracing().
  protos::gen::TraceStats_FinalFlushOutcome final_flush_outcome{};

  // One observation of a producer connection for TraceStats.tracing_v2. See
  // TraceStats.TracingV2 for the window and its limits.
  // - SetupDataSource() starts it for the first instance that requests v2,
  //   before the producer hears of that instance.
  // - Stop, producer disconnect and clone end it: |final_entry| is then set
  //   and never changes. Unregistering a data source does not end it.
  // - A live observation always belongs to the connected producer with
  //   |producer_id|, because DisconnectProducer() ends it before the ID can be
  //   reused. A later connection with the same ID gets a new observation.
  struct TracingV2Observation {
    ProducerID producer_id = 0;
    // BOOTTIME.
    int64_t start_ns = 0;
    TraceStats::TracingV2::Counters counters_at_start;
    uint32_t instances_requesting_v2 = 0;
    uint32_t instances_eligible_for_v2 = 0;
    // REASON_REJECTED_CHUNK dumps taken for this observation, and the ones
    // skipped after kMaxRejectedChunkDumps.
    uint32_t rejected_chunk_dumps = 0;
    uint32_t rejected_chunk_dumps_suppressed = 0;
    // The entry at the end. Empty while the observation is live.
    std::optional<TraceStats::TracingV2::Producer> final_entry;
  };
  // In start order. Live observations are bounded by the connected
  // producers. Observations ended by a disconnect are bounded by eviction.
  std::vector<TracingV2Observation> tracing_v2_observations;
  // Observations ended by a disconnect that were removed to keep the newest.
  uint32_t tracing_v2_producers_evicted = 0;

  // TracingV2RingBufferDump packets that the next reads emit, oldest first.
  struct PendingTracingV2Dump {
    // A serialized TracePacket.
    std::string packet;
    // REASON_PROTOCOL_ERROR or REASON_REJECTED_CHUNK. Such a dump evicts other
    // dumps first.
    bool is_error = false;
  };
  std::vector<PendingTracingV2Dump> pending_tracing_v2_dumps;
  // The sum of the packet sizes, at most kMaxPendingTracingV2DumpBytes.
  size_t pending_tracing_v2_dump_bytes = 0;
  // See TraceStats.TracingV2.
  uint32_t tracing_v2_dumps_evicted = 0;
  uint32_t tracing_v2_dumps_omitted = 0;
  uint32_t tracing_v2_dumps_over_file_size = 0;

  // Set to true on the first call to MaybeNotifyAllDataSourcesStarted().
  bool did_notify_all_data_source_started = false;

  // Stores simple lifecycle events of a particular type (i.e. associated with
  // a single field id in the TracingServiceEvent proto).
  struct LifecycleEvent {
    explicit LifecycleEvent(uint32_t f_id, uint32_t m_size = 1)
        : field_id(f_id), max_size(m_size), timestamps(m_size) {}

    // The field id of the event in the TracingServiceEvent proto.
    uint32_t field_id;

    // Stores the max size of |timestamps|. Set to 1 by default (in
    // the constructor) but can be overridden in TraceSession constructor
    // if a larger size is required.
    uint32_t max_size;

    // Stores the timestamps emitted for each event type (in nanoseconds).
    // Emitted into the trace and cleared when the consumer next calls
    // ReadBuffers.
    base::CircularQueue<int64_t> timestamps;
  };
  std::vector<LifecycleEvent> lifecycle_events;

  // Stores arbitrary lifecycle events that don't fit in lifecycle_events as
  // serialized TracePacket protos.
  struct ArbitraryLifecycleEvent {
    int64_t timestamp;
    std::vector<uint8_t> data;
  };

  std::optional<ArbitraryLifecycleEvent> slow_start_event;

  std::vector<ArbitraryLifecycleEvent> last_flush_events;

  // If this is a cloned tracing session, the timestamp at which each buffer
  // was cloned.
  std::vector<int64_t> buffer_cloned_timestamps;

  using ClockSnapshotData = base::ClockSnapshotVector;

  // Initial clock snapshot, captured at trace start time (when state goes to
  // TracingSession::STARTED). Emitted into the trace when the consumer first
  // calls ReadBuffers().
  ClockSnapshotData initial_clock_snapshot;

  // Stores clock snapshots to emit into the trace as a ring buffer. This
  // buffer is populated both periodically and when lifecycle events happen
  // but only when significant clock drift is detected. Emitted into the trace
  // and cleared when the consumer next calls ReadBuffers().
  base::CircularQueue<ClockSnapshotData> clock_snapshot_ring_buffer;

  State state = DISABLED;

  // BOOTTIME (ns) when this session entered its current state. Maintained
  // solely by SetSessionState(), which every state transition goes through.
  // Used as the timestamp when this session is snapshotted into another
  // session's concurrent_session_events.
  int64_t current_state_start_ns = 0;

  // State changes of other concurrently active sessions. Emitted as
  // ConcurrentSessionEvent packets and cleared on ReadBuffers(), like
  // lifecycle_events.
  struct ConcurrentSessionEvent {
    int64_t timestamp = 0;            // BOOTTIME (ns) of the state change.
    TracingSessionID session_id = 0;  // Id of the session that changed state.
    State state = DISABLED;           // The state it transitioned to.
    uid_t consumer_uid = 0;           // Uid of its consumer.
    uint32_t num_data_sources = 0;    // Its data source count.
    std::string name;                 // Its unique_session_name (or empty).
  };

  std::vector<ConcurrentSessionEvent> concurrent_session_events;

  // If the consumer detached the session, this variable defines the key used
  // for identifying the session later when reattaching.
  std::string detach_key;

  // This is set when the Consumer calls sets |write_into_file| == true in the
  // TraceConfig. In this case this represents the file we should stream the
  // trace packets into, rather than returning it to the consumer via
  // OnTraceData().
  base::ScopedFile write_into_file;
  uint32_t write_period_ms = 0;

  // Flush strategy for the tracing session:
  // * kDisabled: default, no periodic or on-write flushing is performed.
  // * kOnWrite: Buffers are flushed every time data is written to the output
  // file.
  // * kPeriodic: Buffers are flushed periodically based on the value of
  //              periodic_flush_ms.
  enum class FlushStrategy : uint8_t { kDisabled, kOnWrite, kPeriodic };
  FlushStrategy flush_strategy = FlushStrategy::kDisabled;
  uint32_t periodic_flush_ms = 0;
  bool fflush_post_write = false;
  uint64_t max_file_size_bytes = 0;
  uint64_t bytes_written_into_file = 0;

  // Periodic task for snapshotting service events (e.g. clocks, sync markers
  // etc)
  base::PeriodicTask snapshot_periodic_task;

  // Deferred task that stops the trace when |duration_ms| expires. This is
  // to handle the case of |prefer_suspend_clock_for_duration| which cannot
  // use PostDelayedTask.
  base::PeriodicTask timed_stop_task;

  // When non-NULL the packets should be post-processed using the filter.
  std::unique_ptr<protozero::MessageFilter> trace_filter;
  uint64_t filter_input_packets = 0;
  uint64_t filter_input_bytes = 0;
  uint64_t filter_output_bytes = 0;
  uint64_t filter_errors = 0;
  uint64_t filter_time_taken_ns = 0;
  std::vector<uint64_t> filter_bytes_discarded_per_buffer;

  // A randomly generated trace identifier. Note that this does NOT always
  // match the requested TraceConfig.trace_uuid_msb/lsb. Specifically, it does
  // until a gap-less snapshot is requested. Each snapshot re-generates the
  // uuid to avoid emitting two different traces with the same uuid.
  base::Uuid trace_uuid;

  // This is set when the clone operation was caused by a clone trigger.
  std::optional<TriggerInfo> clone_trigger;

  std::optional<base::ScopedSchedBoost> priority_boost;

  // NOTE: when adding new fields here consider whether that state should be
  // copied over in DoCloneSession() or not. Ask yourself: is this a
  // "runtime state" (e.g. active data sources) or a "trace (meta)data state"?
  // If the latter, it should be handled by DoCloneSession()).
};

}  // namespace tracing_service
}  // namespace perfetto
#endif  // SRC_TRACING_SERVICE_TRACING_SERVICE_SESSION_H_
