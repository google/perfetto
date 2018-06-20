/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_TRACING_CORE_TRACING_SERVICE_IMPL_H_
#define SRC_TRACING_CORE_TRACING_SERVICE_IMPL_H_

#include <functional>
#include <map>
#include <memory>
#include <set>

#include "gtest/gtest_prod.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/page_allocator.h"
#include "perfetto/base/time.h"
#include "perfetto/base/weak_ptr.h"
#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/commit_data_request.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/shared_memory_abi.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/tracing_service.h"
#include "src/tracing/core/id_allocator.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base

class Consumer;
class DataSourceConfig;
class Producer;
class SharedMemory;
class SharedMemoryArbiterImpl;
class TraceBuffer;
class TraceConfig;
class TracePacket;

// The tracing service business logic.
class TracingServiceImpl : public TracingService {
 public:
  static constexpr size_t kDefaultShmSize = 256 * 1024ul;
  static constexpr size_t kMaxShmSize = 32 * 1024 * 1024ul;

  // The implementation behind the service endpoint exposed to each producer.
  class ProducerEndpointImpl : public TracingService::ProducerEndpoint {
   public:
    ProducerEndpointImpl(ProducerID,
                         uid_t uid,
                         TracingServiceImpl*,
                         base::TaskRunner*,
                         Producer*,
                         const std::string& producer_name);
    ~ProducerEndpointImpl() override;

    // TracingService::ProducerEndpoint implementation.
    void RegisterDataSource(const DataSourceDescriptor&) override;
    void UnregisterDataSource(const std::string& name) override;
    void CommitData(const CommitDataRequest&, CommitDataCallback) override;
    void SetSharedMemory(std::unique_ptr<SharedMemory>);

    std::unique_ptr<TraceWriter> CreateTraceWriter(BufferID) override;
    void OnTracingSetup();
    void Flush(FlushRequestID, const std::vector<DataSourceInstanceID>&);
    void CreateDataSourceInstance(DataSourceInstanceID,
                                  const DataSourceConfig&);
    void NotifyFlushComplete(FlushRequestID) override;
    void TearDownDataSource(DataSourceInstanceID);
    SharedMemory* shared_memory() const override;
    size_t shared_buffer_page_size_kb() const override;

   private:
    friend class TracingServiceImpl;
    friend class TracingServiceImplTest;
    ProducerEndpointImpl(const ProducerEndpointImpl&) = delete;
    ProducerEndpointImpl& operator=(const ProducerEndpointImpl&) = delete;
    SharedMemoryArbiterImpl* GetOrCreateShmemArbiter();

    ProducerID const id_;
    const uid_t uid_;
    TracingServiceImpl* const service_;
    base::TaskRunner* const task_runner_;
    Producer* producer_;
    std::unique_ptr<SharedMemory> shared_memory_;
    size_t shared_buffer_page_size_kb_ = 0;
    SharedMemoryABI shmem_abi_;
    size_t shmem_size_hint_bytes_ = 0;
    const std::string name_;

    // This is used only in in-process configurations (mostly tests).
    std::unique_ptr<SharedMemoryArbiterImpl> inproc_shmem_arbiter_;
    PERFETTO_THREAD_CHECKER(thread_checker_)
    base::WeakPtrFactory<ProducerEndpointImpl> weak_ptr_factory_;  // Keep last.
  };

  // The implementation behind the service endpoint exposed to each consumer.
  class ConsumerEndpointImpl : public TracingService::ConsumerEndpoint {
   public:
    ConsumerEndpointImpl(TracingServiceImpl*, base::TaskRunner*, Consumer*);
    ~ConsumerEndpointImpl() override;

    void NotifyOnTracingDisabled();
    base::WeakPtr<ConsumerEndpointImpl> GetWeakPtr();

    // TracingService::ConsumerEndpoint implementation.
    void EnableTracing(const TraceConfig&, base::ScopedFile) override;
    void DisableTracing() override;
    void ReadBuffers() override;
    void FreeBuffers() override;
    void Flush(uint32_t timeout_ms, FlushCallback) override;

   private:
    friend class TracingServiceImpl;
    ConsumerEndpointImpl(const ConsumerEndpointImpl&) = delete;
    ConsumerEndpointImpl& operator=(const ConsumerEndpointImpl&) = delete;

    base::TaskRunner* const task_runner_;
    TracingServiceImpl* const service_;
    Consumer* const consumer_;
    TracingSessionID tracing_session_id_ = 0;
    PERFETTO_THREAD_CHECKER(thread_checker_)
    base::WeakPtrFactory<ConsumerEndpointImpl> weak_ptr_factory_;  // Keep last.
  };

  explicit TracingServiceImpl(std::unique_ptr<SharedMemory::Factory>,
                              base::TaskRunner*);
  ~TracingServiceImpl() override;

  // Called by ProducerEndpointImpl.
  void DisconnectProducer(ProducerID);
  void RegisterDataSource(ProducerID, const DataSourceDescriptor&);
  void UnregisterDataSource(ProducerID, const std::string& name);
  void CopyProducerPageIntoLogBuffer(ProducerID,
                                     uid_t,
                                     WriterID,
                                     ChunkID,
                                     BufferID,
                                     uint16_t num_fragments,
                                     uint8_t chunk_flags,
                                     const uint8_t* src,
                                     size_t size);
  void ApplyChunkPatches(ProducerID,
                         const std::vector<CommitDataRequest::ChunkToPatch>&);
  void NotifyFlushDoneForProducer(ProducerID, FlushRequestID);

  // Called by ConsumerEndpointImpl.
  void DisconnectConsumer(ConsumerEndpointImpl*);
  bool EnableTracing(ConsumerEndpointImpl*,
                     const TraceConfig&,
                     base::ScopedFile);
  void DisableTracing(TracingSessionID);
  void Flush(TracingSessionID tsid,
             uint32_t timeout_ms,
             ConsumerEndpoint::FlushCallback);
  void FlushAndDisableTracing(TracingSessionID);
  void ReadBuffers(TracingSessionID, ConsumerEndpointImpl*);
  void FreeBuffers(TracingSessionID);

  // Service implementation.
  std::unique_ptr<TracingService::ProducerEndpoint> ConnectProducer(
      Producer*,
      uid_t uid,
      const std::string& producer_name,
      size_t shared_memory_size_hint_bytes = 0) override;

  std::unique_ptr<TracingService::ConsumerEndpoint> ConnectConsumer(
      Consumer*) override;

  // Exposed mainly for testing.
  size_t num_producers() const { return producers_.size(); }
  ProducerEndpointImpl* GetProducer(ProducerID) const;

 private:
  friend class TracingServiceImplTest;

  struct RegisteredDataSource {
    ProducerID producer_id;
    DataSourceDescriptor descriptor;
  };

  // Represents an active data source for a tracing session.
  struct DataSourceInstance {
    DataSourceInstanceID instance_id;
    std::string data_source_name;
  };

  struct PendingFlush {
    std::set<ProducerID> producers;
    ConsumerEndpoint::FlushCallback callback;
    explicit PendingFlush(decltype(callback) cb) : callback(std::move(cb)) {}
  };

  // Holds the state of a tracing session. A tracing session is uniquely bound
  // a specific Consumer. Each Consumer can own one or more sessions.
  struct TracingSession {
    TracingSession(ConsumerEndpointImpl*, const TraceConfig&);

    size_t num_buffers() const { return buffers_index.size(); }

    uint32_t delay_to_next_write_period_ms() const {
      PERFETTO_DCHECK(write_period_ms > 0);
      return write_period_ms -
             (base::GetWallTimeMs().count() % write_period_ms);
    }

    // The consumer that started the session.
    ConsumerEndpointImpl* const consumer;

    // The original trace config provided by the Consumer when calling
    // EnableTracing().
    const TraceConfig config;

    // List of data source instances that have been enabled on the various
    // producers for this tracing session.
    std::multimap<ProducerID, DataSourceInstance> data_source_instances;

    // For each Flush(N) request, keeps track of the set of producers for which
    // we are still awaiting a NotifyFlushComplete(N) ack.
    std::map<FlushRequestID, PendingFlush> pending_flushes;

    // Maps a per-trace-session buffer index into the corresponding global
    // BufferID (shared namespace amongst all consumers). This vector has as
    // many entries as |config.buffers_size()|.
    std::vector<BufferID> buffers_index;

    // When the last clock snapshot was emitted into the output stream.
    base::TimeMillis last_clock_snapshot = {};

    // When the last TraceStats snapshot was emitted into the output stream.
    base::TimeMillis last_stats_snapshot = {};

    // Whether we mirrored the trace config back to the trace output yet.
    bool did_emit_config = false;

    bool tracing_enabled = false;

    // This is set when the Consumer calls sets |write_into_file| == true in the
    // TraceConfig. In this case this represents the file we should stream the
    // trace packets into, rather than returning it to the consumer via
    // OnTraceData().
    base::ScopedFile write_into_file;
    uint32_t write_period_ms = 0;
    uint64_t max_file_size_bytes = 0;
    uint64_t bytes_written_into_file = 0;
  };

  TracingServiceImpl(const TracingServiceImpl&) = delete;
  TracingServiceImpl& operator=(const TracingServiceImpl&) = delete;

  void CreateDataSourceInstance(const TraceConfig::DataSource&,
                                const TraceConfig::ProducerConfig&,
                                const RegisteredDataSource&,
                                TracingSession*);

  // Returns the next available ProducerID that is not in |producers_|.
  ProducerID GetNextProducerID();

  // Returns a pointer to the |tracing_sessions_| entry or nullptr if the
  // session doesn't exists.
  TracingSession* GetTracingSession(TracingSessionID);

  // Update the memory guard rail by using the latest information from the
  // shared memory and trace buffers.
  void UpdateMemoryGuardrail();

  void MaybeSnapshotClocks(TracingSession*, std::vector<TracePacket>*);
  void MaybeEmitTraceConfig(TracingSession*, std::vector<TracePacket>*);
  void MaybeSnapshotStats(TracingSession*, std::vector<TracePacket>*);
  void OnFlushTimeout(TracingSessionID, FlushRequestID);
  TraceBuffer* GetBufferByID(BufferID);

  base::TaskRunner* const task_runner_;
  std::unique_ptr<SharedMemory::Factory> shm_factory_;
  ProducerID last_producer_id_ = 0;
  DataSourceInstanceID last_data_source_instance_id_ = 0;
  TracingSessionID last_tracing_session_id_ = 0;
  FlushRequestID last_flush_request_id_ = 0;
  uid_t uid_ = 0;

  // Buffer IDs are global across all consumers (because a Producer can produce
  // data for more than one trace session, hence more than one consumer).
  IdAllocator<BufferID> buffer_ids_;

  std::multimap<std::string /*name*/, RegisteredDataSource> data_sources_;
  std::map<ProducerID, ProducerEndpointImpl*> producers_;
  std::set<ConsumerEndpointImpl*> consumers_;
  std::map<TracingSessionID, TracingSession> tracing_sessions_;
  std::map<BufferID, std::unique_ptr<TraceBuffer>> buffers_;

  bool lockdown_mode_ = false;

  PERFETTO_THREAD_CHECKER(thread_checker_)

  base::WeakPtrFactory<TracingServiceImpl>
      weak_ptr_factory_;  // Keep at the end.
};

}  // namespace perfetto

#endif  // SRC_TRACING_CORE_TRACING_SERVICE_IMPL_H_
