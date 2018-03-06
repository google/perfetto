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
#include "perfetto/base/utils.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/tracing/core/commit_data_request.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/shared_memory.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "src/tracing/core/packet_stream_validator.h"

#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trusted_packet.pb.h"

// General note: this class must assume that Producers are malicious and will
// try to crash / exploit this class. We can trust pointers because they come
// from the IPC layer, but we should never assume that that the producer calls
// come in the right order or their arguments are sane / within bounds.

namespace perfetto {

using protozero::proto_utils::MakeTagVarInt;
using protozero::proto_utils::ParseVarInt;
using protozero::proto_utils::WriteVarInt;

namespace {
constexpr size_t kDefaultShmSize = base::kPageSize * 64;  // 256 KB.
constexpr size_t kMaxShmSize = base::kPageSize * 1024;    // 4 MB.
constexpr int kMaxBuffersPerConsumer = 128;

constexpr uint64_t kMillisPerHour = 3600000;

// These apply only if enable_extra_guardrails is true.
constexpr uint64_t kMaxTracingDurationMillis = 24 * kMillisPerHour;
constexpr uint64_t kMaxTracingBufferSizeKb = 32 * 1024;
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
      buffer_ids_(kMaxTraceBufferID),
      weak_ptr_factory_(this) {
  PERFETTO_DCHECK(task_runner_);
}

ServiceImpl::~ServiceImpl() {
  // TODO(fmayer): handle teardown of all Producer.
}

std::unique_ptr<Service::ProducerEndpoint> ServiceImpl::ConnectProducer(
    Producer* producer,
    uid_t uid,
    size_t shared_buffer_size_hint_bytes) {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  if (lockdown_mode_ && uid != geteuid()) {
    PERFETTO_DLOG("Lockdown mode. Rejecting producer with UID %ld",
                  static_cast<unsigned long>(uid));
    return nullptr;
  }

  if (producers_.size() >= kMaxProducerID) {
    PERFETTO_DCHECK(false);
    return nullptr;
  }
  const ProducerID id = GetNextProducerID();
  PERFETTO_DLOG("Producer %" PRIu16 " connected", id);
  size_t shm_size = std::min(shared_buffer_size_hint_bytes, kMaxShmSize);
  if (shm_size % base::kPageSize || shm_size < base::kPageSize)
    shm_size = kDefaultShmSize;

  // TODO(primiano): right now Create() will suicide in case of OOM if the mmap
  // fails. We should instead gracefully fail the request and tell the client
  // to go away.
  auto shared_memory = shm_factory_->CreateSharedMemory(shm_size);
  std::unique_ptr<ProducerEndpointImpl> endpoint(new ProducerEndpointImpl(
      id, uid, this, task_runner_, producer, std::move(shared_memory)));
  auto it_and_inserted = producers_.emplace(id, endpoint.get());
  PERFETTO_DCHECK(it_and_inserted.second);
  task_runner_->PostTask(std::bind(&Producer::OnConnect, endpoint->producer_));

  UpdateMemoryGuardrail();
  return std::move(endpoint);
}

void ServiceImpl::DisconnectProducer(ProducerID id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Producer %" PRIu16 " disconnected", id);
  PERFETTO_DCHECK(producers_.count(id));

  for (auto it = data_sources_.begin(); it != data_sources_.end();) {
    auto next = it;
    next++;
    if (it->second.producer_id == id)
      UnregisterDataSource(id, it->second.data_source_id);
    it = next;
  }

  producers_.erase(id);
  UpdateMemoryGuardrail();
}

ServiceImpl::ProducerEndpointImpl* ServiceImpl::GetProducer(
    ProducerID id) const {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto it = producers_.find(id);
  if (it == producers_.end())
    return nullptr;
  return it->second;
}

std::unique_ptr<Service::ConsumerEndpoint> ServiceImpl::ConnectConsumer(
    Consumer* consumer) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Consumer %p connected", reinterpret_cast<void*>(consumer));
  std::unique_ptr<ConsumerEndpointImpl> endpoint(
      new ConsumerEndpointImpl(this, task_runner_, consumer));
  auto it_and_inserted = consumers_.emplace(endpoint.get());
  PERFETTO_DCHECK(it_and_inserted.second);
  task_runner_->PostTask(std::bind(&Consumer::OnConnect, endpoint->consumer_));
  return std::move(endpoint);
}

void ServiceImpl::DisconnectConsumer(ConsumerEndpointImpl* consumer) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Consumer %p disconnected", reinterpret_cast<void*>(consumer));
  PERFETTO_DCHECK(consumers_.count(consumer));

  // TODO(primiano) : Check that this is safe (what happens if there are
  // ReadBuffers() calls posted in the meantime? They need to become noop).
  if (consumer->tracing_session_id_)
    FreeBuffers(consumer->tracing_session_id_);  // Will also DisableTracing().
  consumers_.erase(consumer);
}

void ServiceImpl::EnableTracing(ConsumerEndpointImpl* consumer,
                                const TraceConfig& cfg) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Enabling tracing for consumer %p",
                reinterpret_cast<void*>(consumer));
  if (cfg.lockdown_mode() == TraceConfig::LockdownModeOperation::LOCKDOWN_SET)
    lockdown_mode_ = true;
  if (cfg.lockdown_mode() == TraceConfig::LockdownModeOperation::LOCKDOWN_CLEAR)
    lockdown_mode_ = false;
  if (consumer->tracing_session_id_) {
    PERFETTO_DLOG(
        "A Consumer is trying to EnableTracing() but another tracing session "
        "is already active (forgot a call to FreeBuffers() ?)");
    // TODO(primiano): make this a bool and return failure to the IPC layer.
    return;
  }

  if (cfg.enable_extra_guardrails()) {
    if (cfg.duration_ms() > kMaxTracingDurationMillis) {
      PERFETTO_ELOG("Requested too long trace (%" PRIu32 "ms  > %" PRIu64
                    " ms)",
                    cfg.duration_ms(), kMaxTracingDurationMillis);
      return;
    }
    uint64_t buf_size_sum = 0;
    for (const auto& buf : cfg.buffers())
      buf_size_sum += buf.size_kb();
    if (buf_size_sum > kMaxTracingBufferSizeKb) {
      PERFETTO_ELOG("Requested too large trace buffer (%" PRIu64
                    "kB  > %" PRIu64 " kB)",
                    buf_size_sum, kMaxTracingBufferSizeKb);
      return;
    }
  }

  if (cfg.buffers_size() > kMaxBuffersPerConsumer) {
    PERFETTO_DLOG("Too many buffers configured (%d)", cfg.buffers_size());
    return;  // TODO(primiano): signal failure to the caller.
  }

  // TODO(primiano): This is a workaround to prevent that a producer gets stuck
  // in a state where it stalls by design by having more TraceWriterImpl
  // instances than free pages in the buffer. This is a very fragile heuristic
  // though, because this assumes that each tracing session creates at most one
  // data source instance in each Producer, and each data source has only one
  // TraceWriter.
  if (tracing_sessions_.size() >= kDefaultShmSize / kBufferPageSize / 2) {
    PERFETTO_ELOG("Too many concurrent tracing sesions (%zu)",
                  tracing_sessions_.size());
    // TODO(primiano): make this a bool and return failure to the IPC layer.
    return;
  }

  const TracingSessionID tsid = ++last_tracing_session_id_;
  TracingSession& ts =
      tracing_sessions_.emplace(tsid, TracingSession(cfg)).first->second;

  // Initialize the log buffers.
  bool did_allocate_all_buffers = true;

  // Allocate the trace buffers. Also create a map to translate a consumer
  // relative index (TraceConfig.DataSourceConfig.target_buffer) into the
  // corresponding BufferID, which is a global ID namespace for the service and
  // all producers.
  size_t total_buf_size_kb = 0;
  ts.buffers_index.reserve(cfg.buffers_size());
  for (int i = 0; i < cfg.buffers_size(); i++) {
    const TraceConfig::BufferConfig& buffer_cfg = cfg.buffers()[i];
    BufferID global_id = buffer_ids_.Allocate();
    if (!global_id) {
      did_allocate_all_buffers = false;  // We ran out of indexes.
      break;
    }
    ts.buffers_index.push_back(global_id);
    auto it_and_inserted = buffers_.emplace(global_id, TraceBuffer());
    PERFETTO_DCHECK(it_and_inserted.second);  // buffers_.count(global_id) == 0.
    TraceBuffer& trace_buffer = it_and_inserted.first->second;
    // TODO(primiano): make TraceBuffer::kBufferPageSize dynamic.
    const size_t buf_size = buffer_cfg.size_kb() * 1024u;
    total_buf_size_kb += buffer_cfg.size_kb();
    if (!trace_buffer.Create(buf_size)) {
      did_allocate_all_buffers = false;
      break;
    }
  }
  UpdateMemoryGuardrail();

  // This can happen if either:
  // - All the kMaxTraceBufferID slots are taken.
  // - OOM, or, more relistically, we exhausted virtual memory.
  // In any case, free all the previously allocated buffers and abort.
  // TODO(fmayer): add a test to cover this case, this is quite subtle.
  if (!did_allocate_all_buffers) {
    for (BufferID global_id : ts.buffers_index) {
      buffer_ids_.Free(global_id);
      buffers_.erase(global_id);
    }
    tracing_sessions_.erase(tsid);
    return;  // TODO(primiano): return failure condition?
  }

  consumer->tracing_session_id_ = tsid;

  // Enable the data sources on the producers.
  for (const TraceConfig::DataSource& cfg_data_source : cfg.data_sources()) {
    // Scan all the registered data sources with a matching name.
    auto range = data_sources_.equal_range(cfg_data_source.config().name());
    for (auto it = range.first; it != range.second; it++)
      CreateDataSourceInstance(cfg_data_source, it->second, &ts);
  }

  // Trigger delayed task if the trace is time limited.
  if (cfg.duration_ms()) {
    auto weak_this = weak_ptr_factory_.GetWeakPtr();
    task_runner_->PostDelayedTask(
        [weak_this, tsid] {
          if (weak_this)
            weak_this->DisableTracing(tsid);
        },
        cfg.duration_ms());
  }

  PERFETTO_LOG("Enabled tracing, #sources:%zu, duration:%" PRIu32
               " ms, #buffers:%d, total buffer size:%zu KB, total sessions:%zu",
               cfg.data_sources().size(), cfg.duration_ms(), cfg.buffers_size(),
               total_buf_size_kb, tracing_sessions_.size());
}

// DisableTracing just stops the data sources but doesn't free up any buffer.
// This is to allow the consumer to freeze the buffers (by stopping the trace)
// and then drain the buffers. The actual teardown of the TracingSession happens
// in FreeBuffers().
void ServiceImpl::DisableTracing(TracingSessionID tsid) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  TracingSession* tracing_session = GetTracingSession(tsid);
  if (!tracing_session) {
    // Can happen if the consumer calls this before EnableTracing() or after
    // FreeBuffers().
    PERFETTO_DLOG("Couldn't find tracing session %" PRIu64, tsid);
    return;
  }

  for (const auto& data_source_inst : tracing_session->data_source_instances) {
    const ProducerID producer_id = data_source_inst.first;
    const DataSourceInstanceID ds_inst_id = data_source_inst.second.instance_id;
    ProducerEndpointImpl* producer = GetProducer(producer_id);
    PERFETTO_DCHECK(producer);
    producer->producer_->TearDownDataSourceInstance(ds_inst_id);
  }
  tracing_session->data_source_instances.clear();

  // Deliberately NOT removing the session from |tracing_session_|, it's still
  // needed to call ReadBuffers(). FreeBuffers() will erase() the session.
}

void ServiceImpl::ReadBuffers(TracingSessionID tsid,
                              ConsumerEndpointImpl* consumer) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Reading buffers for session %" PRIu64, tsid);
  TracingSession* tracing_session = GetTracingSession(tsid);
  if (!tracing_session) {
    PERFETTO_DLOG(
        "Consumer invoked ReadBuffers() but no tracing session is active");
    return;  // TODO(primiano): signal failure?
  }
  // TODO(primiano): Most of this code is temporary and we should find a better
  // solution to bookkeep the log buffer (e.g., an allocator-like freelist)
  // rather than leveraging the SharedMemoryABI (which is intended only for the
  // Producer <> Service SMB and not for the TraceBuffer itself).
  auto weak_consumer = consumer->GetWeakPtr();
  for (size_t buf_idx = 0; buf_idx < tracing_session->num_buffers();
       buf_idx++) {
    auto tbuf_iter = buffers_.find(tracing_session->buffers_index[buf_idx]);
    if (tbuf_iter == buffers_.end()) {
      PERFETTO_DCHECK(false);
      continue;
    }
    TraceBuffer& tbuf = tbuf_iter->second;
    SharedMemoryABI& abi = *tbuf.abi;
    for (size_t i = 0; i < tbuf.num_pages(); i++) {
      const size_t page_idx = (i + tbuf.cur_page) % tbuf.num_pages();
      if (abi.is_page_free(page_idx))
        continue;
      const uid_t page_owner = tbuf.get_page_owner(page_idx);
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
          // TODO(fmayer): stitching, look at the flags.
          bool skip = (pack_idx == 0 &&
                       flags & SharedMemoryABI::ChunkHeader::
                                   kFirstPacketContinuesFromPrevChunk) ||
                      (pack_idx == num_packets - 1 &&
                       flags & SharedMemoryABI::ChunkHeader::
                                   kLastPacketContinuesOnNextChunk);

          PERFETTO_DLOG("  #%-3zu len:%" PRIu64 " skip: %d", pack_idx,
                        pack_size, skip);
          if (ptr > chunk.end() - pack_size) {
            PERFETTO_DLOG("out of bounds!");
            break;
          }
          Slices slices;
          slices.emplace_back(ptr, pack_size);
          if (!skip && !PacketStreamValidator::Validate(slices)) {
            PERFETTO_DLOG("Dropping invalid packet");
            skip = true;
          }

          if (!skip) {
            packets->emplace_back();
            for (Slice& validated_slice : slices)
              packets->back().AddSlice(std::move(validated_slice));

            // Append a chunk with the trusted UID of the producer. This can't
            // be spoofed because above we validated that the existing chunks
            // don't contain any trusted UID fields. For added safety we append
            // instead of prepending because according to protobuf semantics, if
            // the same field is encountered multiple times the last instance
            // takes priority. Note that truncated packets are also rejected, so
            // the producer can't give us a partial packet (e.g., a truncated
            // string) which only becomes valid when the UID is appended here.
            protos::TrustedPacket trusted_packet;
            trusted_packet.set_trusted_uid(page_owner);
            uint8_t trusted_buf[16];
            PERFETTO_CHECK(trusted_packet.SerializeToArray(
                &trusted_buf, sizeof(trusted_buf)));
            packets->back().AddSlice(
                Slice::Copy(trusted_buf, trusted_packet.ByteSize()));
          }
          ptr += pack_size;
        }  // for(packet)
        task_runner_->PostTask([weak_consumer, packets]() {
          if (weak_consumer)
            weak_consumer->consumer_->OnTraceData(std::move(*packets),
                                                  true /*has_more*/);
        });
      }  // for(chunk)
    }    // for(page_idx)
  }      // for(buffer_id)
  task_runner_->PostTask([weak_consumer]() {
    if (weak_consumer)
      weak_consumer->consumer_->OnTraceData(std::vector<TracePacket>(),
                                            false /*has_more*/);
  });
}

void ServiceImpl::FreeBuffers(TracingSessionID tsid) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Freeing buffers for session %" PRIu64, tsid);
  TracingSession* tracing_session = GetTracingSession(tsid);
  if (!tracing_session) {
    PERFETTO_DLOG(
        "Consumer invoked FreeBuffers() but no tracing session is active");
    return;  // TODO(primiano): signal failure?
  }
  DisableTracing(tsid);
  for (BufferID buffer_id : tracing_session->buffers_index) {
    buffer_ids_.Free(buffer_id);
    PERFETTO_DCHECK(buffers_.count(buffer_id) == 1);
    buffers_.erase(buffer_id);
  }
  tracing_sessions_.erase(tsid);
  UpdateMemoryGuardrail();

  PERFETTO_LOG("Tracing session %" PRIu64 " ended, total sessions:%zu", tsid,
               tracing_sessions_.size());
}

void ServiceImpl::RegisterDataSource(ProducerID producer_id,
                                     DataSourceID ds_id,
                                     const DataSourceDescriptor& desc) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Producer %" PRIu16
                " registered data source \"%s\", ID: %" PRIu64,
                producer_id, desc.name().c_str(), ds_id);

  PERFETTO_DCHECK(!desc.name().empty());
  auto reg_ds = data_sources_.emplace(
      desc.name(), RegisteredDataSource{producer_id, ds_id, desc});

  // If there are existing tracing sessions, we need to check if the new
  // data source is enabled by any of them.
  if (tracing_sessions_.empty())
    return;

  ProducerEndpointImpl* producer = GetProducer(producer_id);
  if (!producer) {
    PERFETTO_DCHECK(false);
    return;
  }

  for (auto& iter : tracing_sessions_) {
    TracingSession& tracing_session = iter.second;
    for (const TraceConfig::DataSource& cfg_data_source :
         tracing_session.config.data_sources()) {
      if (cfg_data_source.config().name() == desc.name())
        CreateDataSourceInstance(cfg_data_source, reg_ds->second,
                                 &tracing_session);
    }
  }
}

void ServiceImpl::UnregisterDataSource(ProducerID producer_id,
                                       DataSourceID ds_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_CHECK(producer_id);
  PERFETTO_CHECK(ds_id);
  ProducerEndpointImpl* producer = GetProducer(producer_id);
  PERFETTO_DCHECK(producer);
  for (auto& session : tracing_sessions_) {
    auto it = session.second.data_source_instances.begin();
    while (it != session.second.data_source_instances.end()) {
      if (it->first == producer_id && it->second.data_source_id == ds_id) {
        producer->producer_->TearDownDataSourceInstance(it->second.instance_id);
        it = session.second.data_source_instances.erase(it);
      } else {
        ++it;
      }
    }
  }

  for (auto it = data_sources_.begin(); it != data_sources_.end(); ++it) {
    if (it->second.producer_id == producer_id &&
        it->second.data_source_id == ds_id) {
      data_sources_.erase(it);
      return;
    }
  }
  PERFETTO_DLOG("Tried to unregister a non-existent data source %" PRIu64
                " for producer %" PRIu16,
                ds_id, producer_id);
  PERFETTO_DCHECK(false);
}

void ServiceImpl::CreateDataSourceInstance(
    const TraceConfig::DataSource& cfg_data_source,
    const RegisteredDataSource& data_source,
    TracingSession* tracing_session) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  ProducerEndpointImpl* producer = GetProducer(data_source.producer_id);
  PERFETTO_DCHECK(producer);
  // An existing producer that is not ftrace could have registered itself as
  // ftrace, we must not enable it in that case.
  if (lockdown_mode_ && producer->uid_ != getuid()) {
    PERFETTO_DLOG("Lockdown mode: not enabling producer %hu", producer->id_);
    return;
  }
  // TODO(primiano): match against |producer_name_filter| and add tests
  // for registration ordering (data sources vs consumers).

  // Create a copy of the DataSourceConfig specified in the trace config. This
  // will be passed to the producer after translating the |target_buffer| id.
  // The |target_buffer| parameter passed by the consumer in the trace config is
  // relative to the buffers declared in the same trace config. This has to be
  // translated to the global BufferID before passing it to the producers, which
  // don't know anything about tracing sessions and consumers.

  DataSourceConfig ds_config = cfg_data_source.config();  // Deliberate copy.
  ds_config.set_trace_duration_ms(tracing_session->config.duration_ms());
  auto relative_buffer_id = ds_config.target_buffer();
  if (relative_buffer_id >= tracing_session->num_buffers()) {
    PERFETTO_LOG(
        "The TraceConfig for DataSource %s specified a traget_buffer out of "
        "bound (%d). Skipping it.",
        ds_config.name().c_str(), relative_buffer_id);
    return;
  }
  BufferID global_id = tracing_session->buffers_index[relative_buffer_id];
  PERFETTO_DCHECK(global_id);
  ds_config.set_target_buffer(global_id);

  DataSourceInstanceID inst_id = ++last_data_source_instance_id_;
  tracing_session->data_source_instances.emplace(
      producer->id_, DataSourceInstance{inst_id, data_source.data_source_id});
  PERFETTO_DLOG("Starting data source %s with target buffer %" PRIu16,
                ds_config.name().c_str(), global_id);
  producer->producer_->CreateDataSourceInstance(inst_id, ds_config);
}

void ServiceImpl::CopyProducerPageIntoLogBuffer(ProducerID producer_id,
                                                BufferID target_buffer_id,
                                                const uint8_t* src,
                                                size_t size) {
  // TODO(fmayer): right now the page_size in the SMB and the trace_buffers_ can
  // mismatch. Remove the ability to decide the page size on the Producer.

  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto buf_iter = buffers_.find(target_buffer_id);
  if (buf_iter == buffers_.end()) {
    PERFETTO_DLOG("Could not find target buffer %u for producer %" PRIu16,
                  target_buffer_id, producer_id);
    return;
  }
  TraceBuffer& buf = buf_iter->second;

  // TODO(primiano): we should have a set<BufferID> |allowed_target_buffers| in
  // ProducerEndpointImpl to perform ACL checks and prevent that the Producer
  // passes a |target_buffer| which is valid, but that we never asked it to use.
  // Essentially we want to prevent a malicious producer to inject data into a
  // log buffer that has nothing to do with it.

  PERFETTO_DCHECK(size == kBufferPageSize);
  uid_t uid = GetProducer(producer_id)->uid_;
  uint8_t* dst = buf.acquire_next_page(uid);

  // TODO(primiano): use sendfile(). Requires to make the tbuf itself
  // a file descriptor (just use SharedMemory without sharing it).
  PERFETTO_DLOG(
      "Copying page %p from producer %" PRIu16 " into buffer %" PRIu16,
      reinterpret_cast<const void*>(src), producer_id, target_buffer_id);
  memcpy(dst, src, size);
}

ServiceImpl::TracingSession* ServiceImpl::GetTracingSession(
    TracingSessionID tsid) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto it = tsid ? tracing_sessions_.find(tsid) : tracing_sessions_.end();
  if (it == tracing_sessions_.end())
    return nullptr;
  return &it->second;
}

ProducerID ServiceImpl::GetNextProducerID() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_CHECK(producers_.size() < kMaxProducerID);
  do {
    ++last_producer_id_;
  } while (producers_.count(last_producer_id_) || last_producer_id_ == 0);
  PERFETTO_DCHECK(last_producer_id_ > 0 && last_producer_id_ <= kMaxProducerID);
  return last_producer_id_;
}

void ServiceImpl::UpdateMemoryGuardrail() {
#if !PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)
  uint64_t total_buffer_bytes = 0;

  // Sum up all the shared memory buffers.
  for (const auto& id_to_producer : producers_) {
    total_buffer_bytes += id_to_producer.second->shared_memory()->size();
  }

  // Sum up all the trace buffers.
  for (const auto& id_to_buffer : buffers_) {
    total_buffer_bytes += id_to_buffer.second.size;
  }

  // Set the guard rail to 32MB + the sum of all the buffers over a 30 second
  // interval.
  uint64_t guardrail = 32 * 1024 * 1024 + total_buffer_bytes;
  base::Watchdog::GetInstance()->SetMemoryLimit(guardrail, 30 * 1000);
#endif
}

////////////////////////////////////////////////////////////////////////////////
// ServiceImpl::ConsumerEndpointImpl implementation
////////////////////////////////////////////////////////////////////////////////

ServiceImpl::ConsumerEndpointImpl::ConsumerEndpointImpl(ServiceImpl* service,
                                                        base::TaskRunner*,
                                                        Consumer* consumer)
    : service_(service), consumer_(consumer), weak_ptr_factory_(this) {}

ServiceImpl::ConsumerEndpointImpl::~ConsumerEndpointImpl() {
  service_->DisconnectConsumer(this);
  consumer_->OnDisconnect();
}

void ServiceImpl::ConsumerEndpointImpl::EnableTracing(const TraceConfig& cfg) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  service_->EnableTracing(this, cfg);
}

void ServiceImpl::ConsumerEndpointImpl::DisableTracing() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (tracing_session_id_) {
    service_->DisableTracing(tracing_session_id_);
  } else {
    PERFETTO_LOG("Consumer called DisableTracing() but tracing was not active");
  }
}

void ServiceImpl::ConsumerEndpointImpl::ReadBuffers() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (tracing_session_id_) {
    service_->ReadBuffers(tracing_session_id_, this);
  } else {
    PERFETTO_LOG("Consumer called ReadBuffers() but tracing was not active");
  }
}

void ServiceImpl::ConsumerEndpointImpl::FreeBuffers() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (tracing_session_id_) {
    service_->FreeBuffers(tracing_session_id_);
  } else {
    PERFETTO_LOG("Consumer called FreeBuffers() but tracing was not active");
  }
}

base::WeakPtr<ServiceImpl::ConsumerEndpointImpl>
ServiceImpl::ConsumerEndpointImpl::GetWeakPtr() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  return weak_ptr_factory_.GetWeakPtr();
}

////////////////////////////////////////////////////////////////////////////////
// ServiceImpl::ProducerEndpointImpl implementation
////////////////////////////////////////////////////////////////////////////////

ServiceImpl::ProducerEndpointImpl::ProducerEndpointImpl(
    ProducerID id,
    uid_t uid,
    ServiceImpl* service,
    base::TaskRunner* task_runner,
    Producer* producer,
    std::unique_ptr<SharedMemory> shared_memory)
    : id_(id),
      uid_(uid),
      service_(service),
      task_runner_(task_runner),
      producer_(producer),
      shared_memory_(std::move(shared_memory)),
      shmem_abi_(reinterpret_cast<uint8_t*>(shared_memory_->start()),
                 shared_memory_->size(),
                 kBufferPageSize) {
  // TODO(primiano): make the page-size for the SHM dynamic and find a way to
  // communicate that to the Producer (add a field to the
  // InitializeConnectionResponse IPC).
}

ServiceImpl::ProducerEndpointImpl::~ProducerEndpointImpl() {
  service_->DisconnectProducer(id_);
  producer_->OnDisconnect();
}

void ServiceImpl::ProducerEndpointImpl::RegisterDataSource(
    const DataSourceDescriptor& desc,
    RegisterDataSourceCallback callback) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
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
    DataSourceID ds_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_CHECK(ds_id);
  service_->UnregisterDataSource(id_, ds_id);
}

void ServiceImpl::ProducerEndpointImpl::CommitData(
    const CommitDataRequest& req_untrusted) {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  for (const auto& chunks : req_untrusted.chunks_to_move()) {
    const uint32_t page_idx = chunks.page();
    if (page_idx >= shmem_abi_.num_pages())
      continue;  // A buggy or malicious producer.

    if (!shmem_abi_.is_page_complete(page_idx))
      continue;

    // TODO(primiano): implement per-chunk move.
    PERFETTO_DCHECK(chunks.chunk() == 0);

    if (!shmem_abi_.TryAcquireAllChunksForReading(page_idx))
      continue;

    // TODO(fmayer): we should start collecting individual chunks from non fully
    // complete pages after a while.

    // TODO(primiano): in next CL, use chunks.target_buffer() instead.
    service_->CopyProducerPageIntoLogBuffer(
        id_, shmem_abi_.get_target_buffer(page_idx),
        shmem_abi_.page_start(page_idx), shmem_abi_.page_size());

    shmem_abi_.ReleaseAllChunksAsFree(page_idx);
  }
}

SharedMemory* ServiceImpl::ProducerEndpointImpl::shared_memory() const {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  return shared_memory_.get();
}

std::unique_ptr<TraceWriter>
ServiceImpl::ProducerEndpointImpl::CreateTraceWriter(BufferID) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // TODO(primiano): not implemented yet.
  // This code path is hit only in in-process configuration, where tracing
  // Service and Producer are hosted in the same process. It's a use case we
  // want to support, but not too interesting right now.
  PERFETTO_CHECK(false);
}

////////////////////////////////////////////////////////////////////////////////
// ServiceImpl::TraceBuffer implementation
////////////////////////////////////////////////////////////////////////////////

ServiceImpl::TraceBuffer::TraceBuffer() = default;

bool ServiceImpl::TraceBuffer::Create(size_t size_in_bytes) {
  data = base::PageAllocator::AllocateMayFail(size_in_bytes);
  if (!data) {
    PERFETTO_ELOG("Trace buffer allocation failed (size: %zu, page_size: %zu)",
                  size_in_bytes, kBufferPageSize);
    return false;
  }
  size = size_in_bytes;
  abi.reset(new SharedMemoryABI(get_page(0), size_in_bytes, kBufferPageSize));
  PERFETTO_DCHECK(page_owners.empty());
  page_owners.resize(num_pages(), -1);
  return true;
}

ServiceImpl::TraceBuffer::~TraceBuffer() = default;
ServiceImpl::TraceBuffer::TraceBuffer(ServiceImpl::TraceBuffer&&) noexcept =
    default;
ServiceImpl::TraceBuffer& ServiceImpl::TraceBuffer::operator=(
    ServiceImpl::TraceBuffer&&) = default;

////////////////////////////////////////////////////////////////////////////////
// ServiceImpl::TracingSession implementation
////////////////////////////////////////////////////////////////////////////////

ServiceImpl::TracingSession::TracingSession(const TraceConfig& new_config)
    : config(new_config) {}

}  // namespace perfetto
