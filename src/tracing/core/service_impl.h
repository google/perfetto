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

#ifndef SRC_TRACING_CORE_SERVICE_IMPL_H_
#define SRC_TRACING_CORE_SERVICE_IMPL_H_

#include <functional>
#include <map>
#include <memory>
#include <set>

#include "gtest/gtest_prod.h"
#include "perfetto/base/page_allocator.h"
#include "perfetto/base/time.h"
#include "perfetto/base/weak_ptr.h"
#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/commit_data_request.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/service.h"
#include "perfetto/tracing/core/shared_memory_abi.h"
#include "perfetto/tracing/core/trace_config.h"
#include "src/tracing/core/id_allocator.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base

class Consumer;
class DataSourceConfig;
class Producer;
class SharedMemory;
class TraceBuffez;
class TraceConfig;
class TracePacket;

// The tracing service business logic.
class ServiceImpl : public Service {
 public:
  // The implementation behind the service endpoint exposed to each producer.
  class ProducerEndpointImpl : public Service::ProducerEndpoint {
   public:
    ProducerEndpointImpl(ProducerID,
                         uid_t uid,
                         ServiceImpl*,
                         base::TaskRunner*,
                         Producer*);
    ~ProducerEndpointImpl() override;

    // Service::ProducerEndpoint implementation.
    void RegisterDataSource(const DataSourceDescriptor&,
                            RegisterDataSourceCallback) override;
    void UnregisterDataSource(DataSourceID) override;
    void CommitData(const CommitDataRequest&, CommitDataCallback) override;
    void SetSharedMemory(std::unique_ptr<SharedMemory>);

    std::unique_ptr<TraceWriter> CreateTraceWriter(BufferID) override;
    SharedMemory* shared_memory() const override;
    size_t shared_buffer_page_size_kb() const override;

   private:
    friend class ServiceImpl;
    FRIEND_TEST(ServiceImplTest, RegisterAndUnregister);
    ProducerEndpointImpl(const ProducerEndpointImpl&) = delete;
    ProducerEndpointImpl& operator=(const ProducerEndpointImpl&) = delete;

    ProducerID const id_;
    const uid_t uid_;
    ServiceImpl* const service_;
    base::TaskRunner* const task_runner_;
    Producer* producer_;
    std::unique_ptr<SharedMemory> shared_memory_;
    size_t shared_buffer_page_size_kb_ = 0;
    SharedMemoryABI shmem_abi_;
    size_t shared_memory_size_hint_bytes_ = 0;
    DataSourceID last_data_source_id_ = 0;
    PERFETTO_THREAD_CHECKER(thread_checker_)
  };

  // The implementation behind the service endpoint exposed to each consumer.
  class ConsumerEndpointImpl : public Service::ConsumerEndpoint {
   public:
    ConsumerEndpointImpl(ServiceImpl*, base::TaskRunner*, Consumer*);
    ~ConsumerEndpointImpl() override;

    base::WeakPtr<ConsumerEndpointImpl> GetWeakPtr();

    // Service::ConsumerEndpoint implementation.
    void EnableTracing(const TraceConfig&) override;
    void DisableTracing() override;
    void ReadBuffers() override;
    void FreeBuffers() override;

   private:
    friend class ServiceImpl;
    ConsumerEndpointImpl(const ConsumerEndpointImpl&) = delete;
    ConsumerEndpointImpl& operator=(const ConsumerEndpointImpl&) = delete;

    ServiceImpl* const service_;
    Consumer* const consumer_;
    TracingSessionID tracing_session_id_ = 0;

    PERFETTO_THREAD_CHECKER(thread_checker_)

    base::WeakPtrFactory<ConsumerEndpointImpl> weak_ptr_factory_;
  };

  explicit ServiceImpl(std::unique_ptr<SharedMemory::Factory>,
                       base::TaskRunner*);
  ~ServiceImpl() override;

  // Called by ProducerEndpointImpl.
  void DisconnectProducer(ProducerID);
  void RegisterDataSource(ProducerID,
                          DataSourceID,
                          const DataSourceDescriptor&);
  void UnregisterDataSource(ProducerID, DataSourceID);
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

  // Called by ConsumerEndpointImpl.
  void DisconnectConsumer(ConsumerEndpointImpl*);
  void EnableTracing(ConsumerEndpointImpl*, const TraceConfig&);
  void DisableTracing(TracingSessionID);
  void ReadBuffers(TracingSessionID, ConsumerEndpointImpl*);
  void FreeBuffers(TracingSessionID);

  // Service implementation.
  std::unique_ptr<Service::ProducerEndpoint> ConnectProducer(
      Producer*,
      uid_t uid,
      size_t shared_memory_size_hint_bytes = 0) override;

  std::unique_ptr<Service::ConsumerEndpoint> ConnectConsumer(
      Consumer*) override;

  // Exposed mainly for testing.
  size_t num_producers() const { return producers_.size(); }
  ProducerEndpointImpl* GetProducer(ProducerID) const;

 private:
  FRIEND_TEST(ServiceImplTest, ProducerIDWrapping);

  struct RegisteredDataSource {
    ProducerID producer_id;
    DataSourceID data_source_id;
    DataSourceDescriptor descriptor;
  };

  // Represents an active data source for a tracing session.
  struct DataSourceInstance {
    DataSourceInstanceID instance_id;
    DataSourceID data_source_id;
  };

  // Holds the state of a tracing session. A tracing session is uniquely bound
  // a specific Consumer. Each Consumer can own one or more sessions.
  struct TracingSession {
    explicit TracingSession(const TraceConfig&);

    size_t num_buffers() const { return buffers_index.size(); }

    // Retrieves the page size from the trace config.
    size_t GetDesiredPageSizeKb();

    // Retrieves the SHM size from the trace config.
    size_t GetDesiredShmSizeKb();

    // The original trace config provided by the Consumer when calling
    // EnableTracing().
    const TraceConfig config;

    // List of data source instances that have been enabled on the various
    // producers for this tracing session.
    std::multimap<ProducerID, DataSourceInstance> data_source_instances;

    // Maps a per-trace-session buffer index into the corresponding global
    // BufferID (shared namespace amongst all consumers). This vector has as
    // many entries as |config.buffers_size()|.
    std::vector<BufferID> buffers_index;

    // When the last clock snapshot was emitted into the output stream.
    base::TimeMillis last_clock_snapshot = {};
  };

  ServiceImpl(const ServiceImpl&) = delete;
  ServiceImpl& operator=(const ServiceImpl&) = delete;

  void CreateDataSourceInstance(const TraceConfig::DataSource&,
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

  TraceBuffez* GetBufferByID(BufferID);

  base::TaskRunner* const task_runner_;
  std::unique_ptr<SharedMemory::Factory> shm_factory_;
  ProducerID last_producer_id_ = 0;
  DataSourceInstanceID last_data_source_instance_id_ = 0;
  TracingSessionID last_tracing_session_id_ = 0;
  size_t shared_memory_size_hint_bytes_ = 0;

  // Buffer IDs are global across all consumers (because a Producer can produce
  // data for more than one trace session, hence more than one consumer).
  IdAllocator<BufferID> buffer_ids_;

  std::multimap<std::string /*name*/, RegisteredDataSource> data_sources_;

  // TODO(primiano): There doesn't seem to be any good reason why |producers_|
  // is a map indexed by ID and not just a set<ProducerEndpointImpl*>.
  std::map<ProducerID, ProducerEndpointImpl*> producers_;

  std::set<ConsumerEndpointImpl*> consumers_;
  std::map<TracingSessionID, TracingSession> tracing_sessions_;
  std::map<BufferID, std::unique_ptr<TraceBuffez>> buffers_;

  bool lockdown_mode_ = false;

  PERFETTO_THREAD_CHECKER(thread_checker_)

  base::WeakPtrFactory<ServiceImpl> weak_ptr_factory_;  // Keep at the end.
};

}  // namespace perfetto

#endif  // SRC_TRACING_CORE_SERVICE_IMPL_H_
