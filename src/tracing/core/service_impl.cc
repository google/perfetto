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

#include "src/tracing/core/service_impl.h"

#include <inttypes.h>
#include <string.h>

#include <algorithm>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/shared_memory.h"
#include "perfetto/tracing/core/trace_packet.h"

namespace perfetto {

// TODO add ThreadChecker everywhere.

using protozero::proto_utils::ParseVarInt;

namespace {
constexpr size_t kPageSize = 4096;
constexpr size_t kDefaultShmSize = kPageSize * 16;  // 64 KB.
constexpr size_t kMaxShmSize = kPageSize * 1024;    // 4 MB.
}  // namespace

// static
std::unique_ptr<Service> Service::CreateInstance(
    std::unique_ptr<SharedMemory::Factory> shm_factory,
    base::TaskRunner* task_runner) {
  return std::unique_ptr<Service>(
      new ServiceImpl(std::move(shm_factory), task_runner));
}

ServiceImpl::ServiceImpl(std::unique_ptr<SharedMemory::Factory> shm_factory,
                         base::TaskRunner* task_runner)
    : task_runner_(task_runner),
      shm_factory_(std::move(shm_factory)),
      buffer_ids_(kMaxTraceBuffers) {
  PERFETTO_DCHECK(task_runner_);
}

ServiceImpl::~ServiceImpl() {
  // TODO handle teardown of all Producer.
}

std::unique_ptr<Service::ProducerEndpoint> ServiceImpl::ConnectProducer(
    Producer* producer,
    size_t shared_buffer_size_hint_bytes) {
  const ProducerID id = ++last_producer_id_;
  PERFETTO_DLOG("Producer %" PRIu64 " connected", id);
  size_t shm_size = std::min(shared_buffer_size_hint_bytes, kMaxShmSize);
  if (shm_size % kPageSize || shm_size < kPageSize)
    shm_size = kDefaultShmSize;

  // TODO(primiano): right now Create() will suicide in case of OOM if the mmap
  // fails. We should instead gracefully fail the request and tell the client
  // to go away.
  auto shared_memory = shm_factory_->CreateSharedMemory(shm_size);
  std::unique_ptr<ProducerEndpointImpl> endpoint(new ProducerEndpointImpl(
      id, this, task_runner_, producer, std::move(shared_memory)));
  auto it_and_inserted = producers_.emplace(id, endpoint.get());
  PERFETTO_DCHECK(it_and_inserted.second);
  task_runner_->PostTask(std::bind(&Producer::OnConnect, endpoint->producer()));
  return std::move(endpoint);
}

void ServiceImpl::DisconnectProducer(ProducerID id) {
  PERFETTO_DLOG("Producer %" PRIu64 " disconnected", id);
  PERFETTO_DCHECK(producers_.count(id));
  producers_.erase(id);
}

ServiceImpl::ProducerEndpointImpl* ServiceImpl::GetProducer(
    ProducerID id) const {
  auto it = producers_.find(id);
  if (it == producers_.end())
    return nullptr;
  return it->second;
}

std::unique_ptr<Service::ConsumerEndpoint> ServiceImpl::ConnectConsumer(
    Consumer* consumer) {
  PERFETTO_DLOG("Consumer %p connected", reinterpret_cast<void*>(consumer));
  std::unique_ptr<ConsumerEndpointImpl> endpoint(
      new ConsumerEndpointImpl(this, task_runner_, consumer));
  auto it_and_inserted = consumers_.emplace(endpoint.get());
  PERFETTO_DCHECK(it_and_inserted.second);
  task_runner_->PostTask(std::bind(&Consumer::OnConnect, endpoint->consumer()));
  return std::move(endpoint);
}

void ServiceImpl::DisconnectConsumer(ConsumerEndpointImpl* consumer) {
  PERFETTO_DLOG("Consumer %p disconnected", reinterpret_cast<void*>(consumer));
  PERFETTO_DCHECK(consumers_.count(consumer));
  // TODO: In next CL, tear down the trace sessions for the consumer.
  consumers_.erase(consumer);
}

void ServiceImpl::EnableTracing(ConsumerEndpointImpl* consumer,
                                const TraceConfig& cfg) {
  PERFETTO_DLOG("Enabling tracing for consumer %p",
                reinterpret_cast<void*>(consumer));

  if (tracing_sessions_.count(consumer)) {
    PERFETTO_DLOG(
        "A Consumer is trying to EnableTracing() but another tracing session "
        "is already active");
    // TODO(primiano): make this a bool and return failure to the IPC layer.
    return;
  }
  TracingSession& ts =
      tracing_sessions_.emplace(consumer, TracingSession{cfg}).first->second;

  // Initialize the log buffers.
  bool did_allocate_all_buffers = true;
  for (const TraceConfig::BufferConfig& buffer_cfg : cfg.buffers()) {
    BufferID id = static_cast<BufferID>(buffer_ids_.Allocate());
    if (!id) {
      did_allocate_all_buffers = false;
      break;
    }
    PERFETTO_DCHECK(ts.trace_buffers.count(id) == 0);
    // TODO(primiano): make TraceBuffer::kBufferPageSize dynamic.
    const size_t buf_size = buffer_cfg.size_kb() * 1024u;
    auto it_and_inserted = ts.trace_buffers.emplace(id, TraceBuffer(buf_size));
    if (!it_and_inserted.second || !it_and_inserted.first->second.is_valid()) {
      did_allocate_all_buffers = false;
      break;
    }
  }

  // This can happen if all the kMaxTraceBuffers slots are taken (i.e. we are
  // not talking about OOM here, just creating > kMaxTraceBuffers entries). In
  // this case, free all the previously allocated buffers and abort.
  // TODO: add a test to cover this case, this is quite subtle.
  if (!did_allocate_all_buffers) {
    for (const auto& kv : ts.trace_buffers)
      buffer_ids_.Free(kv.first);
    ts.trace_buffers.clear();
    return;  // TODO(primiano): return failure condition?
  }

  // Enable the data sources on the producers.
  for (const TraceConfig::DataSource& cfg_data_source : cfg.data_sources()) {
    // Scan all the registered data sources with a matching name.
    auto range = data_sources_.equal_range(cfg_data_source.config().name());
    for (auto it = range.first; it != range.second; it++) {
      const RegisteredDataSource& reg_data_source = it->second;

      auto producer_iter = producers_.find(reg_data_source.producer_id);
      if (producer_iter == producers_.end()) {
        PERFETTO_DCHECK(false);  // Something in the unregistration is broken.
        continue;
      }

      CreateDataSourceInstanceForProducer(cfg_data_source,
                                          producer_iter->second, &ts);
    }
  }
}  // namespace perfetto

void ServiceImpl::DisableTracing(ConsumerEndpointImpl* consumer) {
  PERFETTO_DLOG("Disabling tracing for consumer %p",
                reinterpret_cast<void*>(consumer));
  auto it = tracing_sessions_.find(consumer);
  if (it == tracing_sessions_.end()) {
    PERFETTO_DLOG("No active tracing session found for the Consumer");
    return;
  }
  TracingSession& tracing_session = it->second;
  for (const auto& data_source_inst : tracing_session.data_source_instances) {
    auto producer_it = producers_.find(data_source_inst.first);
    if (producer_it == producers_.end())
      continue;  // This could legitimately happen if a Producer disconnects.
    producer_it->second->producer()->TearDownDataSourceInstance(
        data_source_inst.second);
  }
  tracing_session.data_source_instances.clear();
}

void ServiceImpl::ReadBuffers(ConsumerEndpointImpl* consumer) {
  PERFETTO_DLOG("Reading buffers for consumer %p",
                reinterpret_cast<void*>(consumer));
  auto it = tracing_sessions_.find(consumer);
  if (it == tracing_sessions_.end()) {
    PERFETTO_DLOG(
        "Consumer invoked ReadBuffers() but no tracing session is active");
    return;  // TODO(primiano): signal failure?
  }
  // TODO(primiano): Most of this code is temporary and we should find a better
  // solution to bookkeep the log buffer (e.g., an allocator-like freelist)
  // rather than leveraging the SharedMemoryABI (which is intended only for the
  // Producer <> Service SMB and not for the TraceBuffer itself).
  auto weak_consumer = consumer->GetWeakPtr();
  for (auto& buf_it : it->second.trace_buffers) {
    TraceBuffer& tbuf = buf_it.second;
    SharedMemoryABI& abi = *tbuf.abi;
    for (size_t i = 0; i < tbuf.num_pages(); i++) {
      const size_t page_idx = (i + tbuf.cur_page) % tbuf.num_pages();
      if (abi.is_page_free(page_idx))
        continue;
      uint32_t layout = abi.page_layout_dbg(page_idx);
      size_t num_chunks = abi.GetNumChunksForLayout(layout);
      for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
        if (abi.GetChunkState(page_idx, chunk_idx) ==
            SharedMemoryABI::kChunkFree) {
          continue;
        }
        auto chunk = abi.GetChunkUnchecked(page_idx, layout, chunk_idx);
        uint16_t num_packets;
        uint8_t flags;
        std::tie(num_packets, flags) = chunk.GetPacketCountAndFlags();
        const uint8_t* ptr = chunk.payload_begin();

        // shared_ptr is really a workardound for the fact that is not possible
        // to std::move() move-only types in labmdas until C++17.
        std::shared_ptr<std::vector<TracePacket>> packets(
            new std::vector<TracePacket>());
        packets->reserve(num_packets);

        for (size_t pack_idx = 0; pack_idx < num_packets; pack_idx++) {
          uint64_t pack_size = 0;
          ptr = ParseVarInt(ptr, chunk.end(), &pack_size);
          // TODO stitching, look at the flags.
          bool skip = (pack_idx == 0 &&
                       flags & SharedMemoryABI::ChunkHeader::
                                   kFirstPacketContinuesFromPrevChunk) ||
                      (pack_idx == num_packets - 1 &&
                       flags & SharedMemoryABI::ChunkHeader::
                                   kLastPacketContinuesOnNextChunk);

          PERFETTO_DLOG("  #%-3zu len:%" PRIu64 " skip: %d\n", pack_idx,
                        pack_size, skip);
          if (ptr > chunk.end() - pack_size) {
            PERFETTO_DLOG("out of bounds!\n");
            break;
          }
          if (!skip) {
            packets->emplace_back();
            packets->back().AddChunk(Chunk(ptr, pack_size));
          }
          ptr += pack_size;
        }  // for(packet)
        task_runner_->PostTask([weak_consumer, packets]() {
          if (weak_consumer)
            weak_consumer->consumer()->OnTraceData(std::move(*packets),
                                                   true /*has_more*/);
        });
      }  // for(chunk)
    }    // for(page_idx)
  }      // for(buffer_id)
  task_runner_->PostTask([weak_consumer]() {
    if (weak_consumer)
      weak_consumer->consumer()->OnTraceData(std::vector<TracePacket>(),
                                             false /*has_more*/);
  });
}

void ServiceImpl::FreeBuffers(ConsumerEndpointImpl*) {
  // TODO(primiano): implement here.
  PERFETTO_DLOG("FreeBuffers() not implemented yet");
}

void ServiceImpl::RegisterDataSource(ProducerID producer_id,
                                     DataSourceID ds_id,
                                     const DataSourceDescriptor& desc) {
  PERFETTO_DLOG("Producer %" PRIu64
                " registered data source \"%s\", ID: %" PRIu64,
                producer_id, desc.name().c_str(), ds_id);

  PERFETTO_DCHECK(!desc.name().empty());
  data_sources_.emplace(desc.name(),
                        RegisteredDataSource{producer_id, ds_id, desc});

  // If there's existing tracing sessions, we need to check if the new
  // data source is enabled by any of them.
  if (tracing_sessions_.empty())
    return;

  auto producer_iter = producers_.find(producer_id);
  if (producer_iter == producers_.end()) {
    PERFETTO_DCHECK(false);
    return;
  }

  ProducerEndpointImpl* producer = producer_iter->second;

  for (auto& iter : tracing_sessions_) {
    TracingSession& tracing_session = iter.second;
    for (const TraceConfig::DataSource& cfg_data_source :
         tracing_session.config.data_sources()) {
      if (cfg_data_source.config().name() == desc.name())
        CreateDataSourceInstanceForProducer(cfg_data_source, producer,
                                            &tracing_session);
    }
  }
}

void ServiceImpl::CreateDataSourceInstanceForProducer(
    const TraceConfig::DataSource& cfg_data_source,
    ProducerEndpointImpl* producer,
    TracingSession* tracing_session) {
  // TODO(primiano): match against |producer_name_filter| and add tests
  // for registration ordering (data sources vs consumers).

  DataSourceInstanceID inst_id = ++last_data_source_instance_id_;
  tracing_session->data_source_instances.emplace(producer->id(), inst_id);
  producer->producer()->CreateDataSourceInstance(inst_id,
                                                 cfg_data_source.config());
}

void ServiceImpl::CopyProducerPageIntoLogBuffer(ProducerID producer_id,
                                                BufferID target_buffer,
                                                const uint8_t* src,
                                                size_t size) {
  // TODO right now the page_size in the SMB and the trace_buffers_ can
  // mismatch. Remove the ability to decide the page size on the Producer.

  // TODO(primiano): We should have a direct index to find the TargetBuffer and
  // perform ACL checks without iterating through all the producers.
  TraceBuffer* tbuf = nullptr;
  for (auto& sessions_it : tracing_sessions_) {
    for (auto& tbuf_it : sessions_it.second.trace_buffers) {
      const BufferID id = tbuf_it.first;
      if (id == target_buffer) {
        // TODO(primiano): we should have some stronger check to prevent that
        // the Producer passes |target_buffer| which is valid, but that we never
        // asked it to use. Essentially we want to prevent a malicious producer
        // to inject data into a log buffer that has nothing to do with it.
        tbuf = &tbuf_it.second;
        break;
      }
    }
  }

  if (!tbuf) {
    PERFETTO_DLOG("Could not find target buffer %u for producer %" PRIu64,
                  target_buffer, producer_id);
    return;
  }

  PERFETTO_DCHECK(size == TraceBuffer::kBufferPageSize);
  uint8_t* dst = tbuf->get_next_page();

  // TODO(primiano): use sendfile(). Requires to make the tbuf itself
  // a file descriptor (just use SharedMemory without sharing it).
  PERFETTO_DLOG("Copying page %p from producer %" PRIu64,
                reinterpret_cast<const void*>(src), producer_id);
  memcpy(dst, src, size);
}

////////////////////////////////////////////////////////////////////////////////
// ServiceImpl::ConsumerEndpointImpl implementation
////////////////////////////////////////////////////////////////////////////////

ServiceImpl::ConsumerEndpointImpl::ConsumerEndpointImpl(ServiceImpl* service,
                                                        base::TaskRunner*,
                                                        Consumer* consumer)
    : service_(service), consumer_(consumer), weak_ptr_factory_(this) {}

ServiceImpl::ConsumerEndpointImpl::~ConsumerEndpointImpl() {
  consumer_->OnDisconnect();
  service_->DisconnectConsumer(this);
}

void ServiceImpl::ConsumerEndpointImpl::EnableTracing(const TraceConfig& cfg) {
  service_->EnableTracing(this, cfg);
}

void ServiceImpl::ConsumerEndpointImpl::DisableTracing() {
  service_->DisableTracing(this);
}

void ServiceImpl::ConsumerEndpointImpl::ReadBuffers() {
  service_->ReadBuffers(this);
}

void ServiceImpl::ConsumerEndpointImpl::FreeBuffers() {
  service_->FreeBuffers(this);
}

base::WeakPtr<ServiceImpl::ConsumerEndpointImpl>
ServiceImpl::ConsumerEndpointImpl::GetWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
}

////////////////////////////////////////////////////////////////////////////////
// ServiceImpl::ProducerEndpointImpl implementation
////////////////////////////////////////////////////////////////////////////////

ServiceImpl::ProducerEndpointImpl::ProducerEndpointImpl(
    ProducerID id,
    ServiceImpl* service,
    base::TaskRunner* task_runner,
    Producer* producer,
    std::unique_ptr<SharedMemory> shared_memory)
    : id_(id),
      service_(service),
      task_runner_(task_runner),
      producer_(std::move(producer)),
      shared_memory_(std::move(shared_memory)),
      shmem_abi_(reinterpret_cast<uint8_t*>(shared_memory_->start()),
                 shared_memory_->size(),
                 kPageSize) {
  // TODO(primiano): make the page-size for the SHM dynamic and find a way to
  // communicate that to the Producer (add a field to the
  // InitializeConnectionResponse IPC).
}

ServiceImpl::ProducerEndpointImpl::~ProducerEndpointImpl() {
  producer_->OnDisconnect();
  service_->DisconnectProducer(id_);
}

void ServiceImpl::ProducerEndpointImpl::RegisterDataSource(
    const DataSourceDescriptor& desc,
    RegisterDataSourceCallback callback) {
  DataSourceID ds_id = ++last_data_source_id_;
  if (!desc.name().empty()) {
    service_->RegisterDataSource(id_, ds_id, desc);
  } else {
    PERFETTO_DLOG("Received RegisterDataSource() with empty name");
    ds_id = 0;
  }
  task_runner_->PostTask(std::bind(std::move(callback), ds_id));
}

void ServiceImpl::ProducerEndpointImpl::UnregisterDataSource(
    DataSourceID dsid) {
  PERFETTO_CHECK(dsid);
  // TODO(primiano): implement the bookkeeping logic.
}

void ServiceImpl::ProducerEndpointImpl::NotifySharedMemoryUpdate(
    const std::vector<uint32_t>& changed_pages) {
  for (uint32_t page_idx : changed_pages) {
    if (page_idx >= shmem_abi_.num_pages())
      continue;  // Very likely a malicious producer playing dirty.

    if (!shmem_abi_.is_page_complete(page_idx))
      continue;
    if (!shmem_abi_.TryAcquireAllChunksForReading(page_idx))
      continue;

    // TODO: we should start collecting individual chunks from non fully
    // complete pages after a while.

    service_->CopyProducerPageIntoLogBuffer(
        id_, shmem_abi_.get_target_buffer(page_idx),
        shmem_abi_.page_start(page_idx), shmem_abi_.page_size());

    shmem_abi_.ReleaseAllChunksAsFree(page_idx);
  }
}

SharedMemory* ServiceImpl::ProducerEndpointImpl::shared_memory() const {
  return shared_memory_.get();
}

std::unique_ptr<TraceWriter>
ServiceImpl::ProducerEndpointImpl::CreateTraceWriter(BufferID) {
  // TODO(primiano): not implemented yet.
  // This code path is hit only in in-process configuration, where tracing
  // Service and Producer are hosted in the same process. It's a use case we
  // want to support, but not too interesting right now.
  PERFETTO_CHECK(false);
}

////////////////////////////////////////////////////////////////////////////////
// ServiceImpl::TraceBuffer implementation
////////////////////////////////////////////////////////////////////////////////

ServiceImpl::TraceBuffer::TraceBuffer(size_t sz) : size(sz) {
  data = base::PageAllocator::AllocateMayFail(size);
  if (!data) {
    PERFETTO_ELOG("Trace buffer allocation failed (size: %zu, page_size: %zu)",
                  size, kBufferPageSize);
    size = 0;
    return;
  }
  abi.reset(new SharedMemoryABI(get_page(0), size, kBufferPageSize));
}

ServiceImpl::TraceBuffer::~TraceBuffer() = default;
ServiceImpl::TraceBuffer::TraceBuffer(ServiceImpl::TraceBuffer&&) noexcept =
    default;
ServiceImpl::TraceBuffer& ServiceImpl::TraceBuffer::operator=(
    ServiceImpl::TraceBuffer&&) = default;

}  // namespace perfetto
