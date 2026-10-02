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

#ifndef SRC_TRACING_IPC_PRODUCER_PRODUCER_IPC_CLIENT_IMPL_H_
#define SRC_TRACING_IPC_PRODUCER_PRODUCER_IPC_CLIENT_IMPL_H_

#include <stdint.h>

#include <set>
#include <vector>

#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/ipc/client.h"
#include "perfetto/ext/ipc/service_proxy.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/shared_memory.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/ext/tracing/ipc/producer_ipc_client.h"
#include "src/tracing/v2/producer_ring_buffer_arbiter.h"

#include "protos/perfetto/ipc/producer_port.ipc.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base

namespace test {
class ProducerIPCClientTestPeer;
}  // namespace test

class Producer;
class SharedMemoryArbiter;

// Exposes a Service endpoint to Producer(s), proxying all requests through a
// IPC channel to the remote Service. This class is the glue layer between the
// generic Service interface exposed to the clients of the library and the
// actual IPC transport.
// If create_socket_async is set, it will be called to create and connect to a
// socket to the service. If unset, the producer will create and connect itself.
//
// TODO(sashwinbalaji): Check all paths through Disconnect(), OnDisconnect(),
// and ScheduleDisconnect() again. Check for unnecessary steps, incorrect
// cleanup or callback order, and object lifetime errors.
class ProducerIPCClientImpl : public TracingService::ProducerEndpoint,
                              public ipc::ServiceProxy::EventListener {
 public:
  ProducerIPCClientImpl(ipc::Client::ConnArgs,
                        Producer*,
                        const std::string& producer_name,
                        base::TaskRunner*,
                        TracingService::ProducerSMBScrapingMode,
                        size_t shared_memory_size_hint_bytes,
                        size_t shared_memory_page_size_hint_bytes,
                        std::unique_ptr<SharedMemory> shm,
                        std::unique_ptr<SharedMemoryArbiter> shm_arbiter,
                        CreateSocketAsync create_socket_async);
  ~ProducerIPCClientImpl() override;

  // TracingService::ProducerEndpoint implementation.
  // These methods are invoked by the actual Producer(s) code by clients of the
  // tracing library, which know nothing about the IPC transport.
  void Disconnect() override;
  void RegisterDataSource(const DataSourceDescriptor&) override;
  void UpdateDataSource(const DataSourceDescriptor&) override;
  void UnregisterDataSource(const std::string& name) override;
  void RegisterTraceWriter(uint32_t writer_id, uint32_t target_buffer) override;
  void UnregisterTraceWriter(uint32_t writer_id) override;
  void CommitData(const CommitDataRequest&, CommitDataCallback) override;
  void NotifyDataSourceStarted(DataSourceInstanceID) override;
  void NotifyDataSourceStopped(DataSourceInstanceID) override;
  void ActivateTriggers(const std::vector<std::string>&) override;
  void Sync(std::function<void()> callback) override;
  void AttachV2RingBuffer(const std::shared_ptr<SharedMemory>&,
                          uint32_t chunk_size_bytes,
                          std::function<void(bool)> callback) override;
  void DrainV2RingBuffer() override;

  std::unique_ptr<TraceWriter> CreateTraceWriter(
      BufferID target_buffer,
      BufferExhaustedPolicy) override;
  std::unique_ptr<TraceWriter> CreateTraceWriter(BufferID,
                                                 BufferExhaustedPolicy,
                                                 DataSourceInstanceID) override;
  SharedMemoryArbiter* MaybeSharedMemoryArbiter() override;
  bool IsShmemProvidedByProducer() const override;
  void NotifyFlushComplete(FlushRequestID) override;
  SharedMemory* shared_memory() const override;
  size_t shared_buffer_page_size_kb() const override;

  // ipc::ServiceProxy::EventListener implementation.
  // These methods are invoked by the IPC layer, which knows nothing about
  // tracing, producers and consumers.
  void OnConnect() override;
  void OnDisconnect() override;

  ipc::Client* GetClientForTesting() { return ipc_channel_.get(); }

 private:
  // Defined in tracing_integration_test.cc.
  friend class test::ProducerIPCClientTestPeer;

  // Drops the provider connection if a protocol error was detected while
  // processing an IPC command.
  void ScheduleDisconnect();

  // Invoked soon after having established the connection with the service.
  // |offered_versions| is the bitmask sent in the request.
  void OnConnectionInitialized(
      uint32_t offered_versions,
      ipc::AsyncResult<protos::gen::InitializeConnectionResponse> response);

  bool HasNegotiatedV2Abi() const {
    return !!(protocol_abi_versions_ & kProtocolAbiV2);
  }

  // Invoked when the remote Service sends an IPC to tell us to do something
  // (e.g. start/stop a data source).
  void OnServiceRequest(const protos::gen::GetAsyncCommandResponse&);

  // TODO think to destruction order, do we rely on any specific dtor sequence?
  Producer* const producer_;
  base::TaskRunner* const task_runner_;

  // A callback used to receive the shmem region out of band of the socket.
  std::function<int(void)> receive_shmem_fd_cb_fuchsia_;

  // The object that owns the client socket and takes care of IPC traffic.
  std::unique_ptr<ipc::Client> ipc_channel_;

  // The proxy interface for the producer port of the service. It is bound
  // to |ipc_channel_| and (de)serializes method invocations over the wire.
  std::unique_ptr<protos::gen::ProducerPortProxy> producer_port_;

  // Maps registered TraceWriter IDs to their target buffers as registered
  // by the producer. When using shmem emulation, the producer internally
  // needs the mapping of writer to buffer in order to do producer-side
  // SMB scraping as it cannot rely on the map stored in the tracing
  // service.
  std::map<WriterID, BufferID> writers_for_scraping_;

  std::unique_ptr<SharedMemory> shared_memory_;
  // Bitmask of the versions agreed in InitializeConnection, zero until then.
  // It stays set after a disconnect:
  // - CreateTraceWriter() reads it on data source threads, without a lock.
  // - Those threads can create writers also after a disconnect, because the
  //   owner keeps this endpoint alive until its writers are gone.
  // - So a reset on disconnect would race with those reads.
  uint32_t protocol_abi_versions_ = 0;
  std::unique_ptr<SharedMemoryArbiter> shared_memory_arbiter_;
  size_t shared_buffer_page_size_kb_ = 0;
  std::set<DataSourceInstanceID> data_sources_setup_;
  bool connected_ = false;
  std::string const name_;
  size_t shared_memory_page_size_hint_bytes_ = 0;
  size_t shared_memory_size_hint_bytes_ = 0;
  TracingService::ProducerSMBScrapingMode const smb_scraping_mode_;
  bool is_shmem_provided_by_producer_ = false;
  bool direct_smb_patching_supported_ = false;
  bool use_shmem_emulation_ = false;
  std::vector<std::function<void()>> pending_sync_reqs_;
  // Picks v1 or v2 for each data source instance, and owns the ring buffer.
  // OnConnectionInitialized() creates it if v2 is in the common mask.
  // It is never reset, for the same reason as |protocol_abi_versions_|.
  std::unique_ptr<tracing_v2::ProducerRingBufferArbiter>
      v2_ring_buffer_arbiter_;
  base::WeakPtrFactory<ProducerIPCClientImpl> weak_factory_{this};
  PERFETTO_THREAD_CHECKER(thread_checker_)
};

}  // namespace perfetto

#endif  // SRC_TRACING_IPC_PRODUCER_PRODUCER_IPC_CLIENT_IMPL_H_
