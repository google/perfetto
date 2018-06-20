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

#include "src/tracing/core/tracing_service_impl.h"

#include "perfetto/base/build_config.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <sys/uio.h>
#include <unistd.h>
#endif

#include <algorithm>

#include "perfetto/base/build_config.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/utils.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/shared_memory.h"
#include "perfetto/tracing/core/shared_memory_abi.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "src/tracing/core/packet_stream_validator.h"
#include "src/tracing/core/shared_memory_arbiter_impl.h"
#include "src/tracing/core/trace_buffer.h"

#include "perfetto/trace/clock_snapshot.pb.h"
#include "perfetto/trace/trusted_packet.pb.h"

// General note: this class must assume that Producers are malicious and will
// try to crash / exploit this class. We can trust pointers because they come
// from the IPC layer, but we should never assume that that the producer calls
// come in the right order or their arguments are sane / within bounds.

namespace perfetto {

namespace {
constexpr size_t kDefaultShmPageSize = base::kPageSize;
constexpr int kMaxBuffersPerConsumer = 128;
constexpr base::TimeMillis kClockSnapshotInterval(10 * 1000);
constexpr base::TimeMillis kStatsSnapshotInterval(10 * 1000);
constexpr int kMinWriteIntoFilePeriodMs = 100;
constexpr int kDefaultWriteIntoFilePeriodMs = 5000;
constexpr int kFlushTimeoutMs = 1000;
constexpr int kMaxConcurrentTracingSessions = 5;

constexpr uint64_t kMillisPerHour = 3600000;

// These apply only if enable_extra_guardrails is true.
constexpr uint64_t kMaxTracingDurationMillis = 24 * kMillisPerHour;
constexpr uint64_t kMaxTracingBufferSizeKb = 32 * 1024;

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
struct iovec {
  void* iov_base;  // Address
  size_t iov_len;  // Block size
};

// Simple implementation of writev. Note that this does not give the atomicity
// guarantees of a real writev, but we don't depend on these (we aren't writing
// to the same file from another thread).
ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
  ssize_t total_size = 0;
  for (int i = 0; i < iovcnt; ++i) {
    ssize_t current_size = write(fd, iov[i].iov_base, iov[i].iov_len);
    if (current_size != static_cast<ssize_t>(iov[i].iov_len))
      return -1;
    total_size += current_size;
  }
  return total_size;
}

#define IOV_MAX 1024  // Linux compatible limit.

// uid checking is a NOP on Windows.
uid_t getuid() {
  return 0;
}
uid_t geteuid() {
  return 0;
}
#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
}  // namespace

// These constants instead are defined in the header because are used by tests.
constexpr size_t TracingServiceImpl::kDefaultShmSize;
constexpr size_t TracingServiceImpl::kMaxShmSize;

// static
std::unique_ptr<TracingService> TracingService::CreateInstance(
    std::unique_ptr<SharedMemory::Factory> shm_factory,
    base::TaskRunner* task_runner) {
  return std::unique_ptr<TracingService>(
      new TracingServiceImpl(std::move(shm_factory), task_runner));
}

TracingServiceImpl::TracingServiceImpl(
    std::unique_ptr<SharedMemory::Factory> shm_factory,
    base::TaskRunner* task_runner)
    : task_runner_(task_runner),
      shm_factory_(std::move(shm_factory)),
      uid_(getuid()),
      buffer_ids_(kMaxTraceBufferID),
      weak_ptr_factory_(this) {
  PERFETTO_DCHECK(task_runner_);
}

TracingServiceImpl::~TracingServiceImpl() {
  // TODO(fmayer): handle teardown of all Producer.
}

std::unique_ptr<TracingService::ProducerEndpoint>
TracingServiceImpl::ConnectProducer(Producer* producer,
                                    uid_t uid,
                                    const std::string& producer_name,
                                    size_t shared_memory_size_hint_bytes) {
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

  std::unique_ptr<ProducerEndpointImpl> endpoint(new ProducerEndpointImpl(
      id, uid, this, task_runner_, producer, producer_name));
  auto it_and_inserted = producers_.emplace(id, endpoint.get());
  PERFETTO_DCHECK(it_and_inserted.second);
  endpoint->shmem_size_hint_bytes_ = shared_memory_size_hint_bytes;
  task_runner_->PostTask(std::bind(&Producer::OnConnect, endpoint->producer_));

  return std::move(endpoint);
}

void TracingServiceImpl::DisconnectProducer(ProducerID id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Producer %" PRIu16 " disconnected", id);
  PERFETTO_DCHECK(producers_.count(id));

  for (auto it = data_sources_.begin(); it != data_sources_.end();) {
    auto next = it;
    next++;
    if (it->second.producer_id == id)
      UnregisterDataSource(id, it->second.descriptor.name());
    it = next;
  }

  producers_.erase(id);
  UpdateMemoryGuardrail();
}

TracingServiceImpl::ProducerEndpointImpl* TracingServiceImpl::GetProducer(
    ProducerID id) const {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto it = producers_.find(id);
  if (it == producers_.end())
    return nullptr;
  return it->second;
}

std::unique_ptr<TracingService::ConsumerEndpoint>
TracingServiceImpl::ConnectConsumer(Consumer* consumer) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Consumer %p connected", reinterpret_cast<void*>(consumer));
  std::unique_ptr<ConsumerEndpointImpl> endpoint(
      new ConsumerEndpointImpl(this, task_runner_, consumer));
  auto it_and_inserted = consumers_.emplace(endpoint.get());
  PERFETTO_DCHECK(it_and_inserted.second);
  task_runner_->PostTask(std::bind(&Consumer::OnConnect, endpoint->consumer_));
  return std::move(endpoint);
}

void TracingServiceImpl::DisconnectConsumer(ConsumerEndpointImpl* consumer) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Consumer %p disconnected", reinterpret_cast<void*>(consumer));
  PERFETTO_DCHECK(consumers_.count(consumer));

  // TODO(primiano) : Check that this is safe (what happens if there are
  // ReadBuffers() calls posted in the meantime? They need to become noop).
  if (consumer->tracing_session_id_)
    FreeBuffers(consumer->tracing_session_id_);  // Will also DisableTracing().
  consumers_.erase(consumer);

// At this point no more pointers to |consumer| should be around.
#if PERFETTO_DCHECK_IS_ON()
  PERFETTO_DCHECK(!std::any_of(
      tracing_sessions_.begin(), tracing_sessions_.end(),
      [consumer](const std::pair<const TracingSessionID, TracingSession>& kv) {
        return kv.second.consumer == consumer;
      }));
#endif
}

bool TracingServiceImpl::EnableTracing(ConsumerEndpointImpl* consumer,
                                       const TraceConfig& cfg,
                                       base::ScopedFile fd) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Enabling tracing for consumer %p",
                reinterpret_cast<void*>(consumer));
  if (cfg.lockdown_mode() == TraceConfig::LockdownModeOperation::LOCKDOWN_SET)
    lockdown_mode_ = true;
  if (cfg.lockdown_mode() == TraceConfig::LockdownModeOperation::LOCKDOWN_CLEAR)
    lockdown_mode_ = false;
  TracingSession* tracing_session =
      GetTracingSession(consumer->tracing_session_id_);
  if (tracing_session) {
    PERFETTO_DLOG(
        "A Consumer is trying to EnableTracing() but another tracing session "
        "is already active (forgot a call to FreeBuffers() ?)");
    return false;
  }

  if (cfg.enable_extra_guardrails()) {
    if (cfg.duration_ms() > kMaxTracingDurationMillis) {
      PERFETTO_ELOG("Requested too long trace (%" PRIu32 "ms  > %" PRIu64
                    " ms)",
                    cfg.duration_ms(), kMaxTracingDurationMillis);
      return false;
    }
    uint64_t buf_size_sum = 0;
    for (const auto& buf : cfg.buffers())
      buf_size_sum += buf.size_kb();
    if (buf_size_sum > kMaxTracingBufferSizeKb) {
      PERFETTO_ELOG("Requested too large trace buffer (%" PRIu64
                    "kB  > %" PRIu64 " kB)",
                    buf_size_sum, kMaxTracingBufferSizeKb);
      return false;
    }
  }

  if (cfg.buffers_size() > kMaxBuffersPerConsumer) {
    PERFETTO_DLOG("Too many buffers configured (%d)", cfg.buffers_size());
    return false;
  }

  // TODO(primiano): This is a workaround to prevent that a producer gets stuck
  // in a state where it stalls by design by having more TraceWriterImpl
  // instances than free pages in the buffer. This is really a bug in
  // trace_probes and the way it handles stalls in the shmem buffer.
  if (tracing_sessions_.size() >= kMaxConcurrentTracingSessions) {
    PERFETTO_ELOG("Too many concurrent tracing sesions (%zu)",
                  tracing_sessions_.size());
    return false;
  }

  const TracingSessionID tsid = ++last_tracing_session_id_;
  tracing_session =
      &tracing_sessions_.emplace(tsid, TracingSession(consumer, cfg))
           .first->second;

  if (cfg.write_into_file()) {
    if (!fd) {
      PERFETTO_ELOG(
          "The TraceConfig had write_into_file==true but no fd was passed");
      return false;
    }
    tracing_session->write_into_file = std::move(fd);
    uint32_t write_period_ms = cfg.file_write_period_ms();
    if (write_period_ms == 0)
      write_period_ms = kDefaultWriteIntoFilePeriodMs;
    if (write_period_ms < kMinWriteIntoFilePeriodMs)
      write_period_ms = kMinWriteIntoFilePeriodMs;
    tracing_session->write_period_ms = write_period_ms;
    tracing_session->max_file_size_bytes = cfg.max_file_size_bytes();
    tracing_session->bytes_written_into_file = 0;
  }

  // Initialize the log buffers.
  bool did_allocate_all_buffers = true;

  // Allocate the trace buffers. Also create a map to translate a consumer
  // relative index (TraceConfig.DataSourceConfig.target_buffer) into the
  // corresponding BufferID, which is a global ID namespace for the service and
  // all producers.
  size_t total_buf_size_kb = 0;
  const size_t num_buffers = static_cast<size_t>(cfg.buffers_size());
  tracing_session->buffers_index.reserve(num_buffers);
  for (size_t i = 0; i < num_buffers; i++) {
    const TraceConfig::BufferConfig& buffer_cfg = cfg.buffers()[i];
    BufferID global_id = buffer_ids_.Allocate();
    if (!global_id) {
      did_allocate_all_buffers = false;  // We ran out of IDs.
      break;
    }
    tracing_session->buffers_index.push_back(global_id);
    const size_t buf_size_bytes = buffer_cfg.size_kb() * 1024u;
    total_buf_size_kb += buffer_cfg.size_kb();
    auto it_and_inserted =
        buffers_.emplace(global_id, TraceBuffer::Create(buf_size_bytes));
    PERFETTO_DCHECK(it_and_inserted.second);  // buffers_.count(global_id) == 0.
    std::unique_ptr<TraceBuffer>& trace_buffer = it_and_inserted.first->second;
    if (!trace_buffer) {
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
    for (BufferID global_id : tracing_session->buffers_index) {
      buffer_ids_.Free(global_id);
      buffers_.erase(global_id);
    }
    tracing_sessions_.erase(tsid);
    return false;
  }

  consumer->tracing_session_id_ = tsid;

  // Enable the data sources on the producers.
  for (const TraceConfig::DataSource& cfg_data_source : cfg.data_sources()) {
    // Scan all the registered data sources with a matching name.
    auto range = data_sources_.equal_range(cfg_data_source.config().name());
    for (auto it = range.first; it != range.second; it++) {
      TraceConfig::ProducerConfig producer_config;
      for (auto& config : cfg.producers()) {
        if (GetProducer(it->second.producer_id)->name_ ==
            config.producer_name()) {
          producer_config = config;
          break;
        }
      }
      CreateDataSourceInstance(cfg_data_source, producer_config, it->second,
                               tracing_session);
    }
  }

  // Trigger delayed task if the trace is time limited.
  const uint32_t trace_duration_ms = cfg.duration_ms();
  if (trace_duration_ms > 0) {
    auto weak_this = weak_ptr_factory_.GetWeakPtr();
    task_runner_->PostDelayedTask(
        [weak_this, tsid] {
          if (weak_this)
            weak_this->FlushAndDisableTracing(tsid);
        },
        trace_duration_ms);
  }

  // Start the periodic drain tasks if we should to save the trace into a file.
  if (cfg.write_into_file()) {
    auto weak_this = weak_ptr_factory_.GetWeakPtr();
    task_runner_->PostDelayedTask(
        [weak_this, tsid] {
          if (weak_this)
            weak_this->ReadBuffers(tsid, nullptr);
        },
        tracing_session->delay_to_next_write_period_ms());
  }

  tracing_session->tracing_enabled = true;
  PERFETTO_LOG(
      "Enabled tracing, #sources:%zu, duration:%d ms, #buffers:%d, total "
      "buffer size:%zu KB, total sessions:%zu",
      cfg.data_sources().size(), trace_duration_ms, cfg.buffers_size(),
      total_buf_size_kb, tracing_sessions_.size());
  return true;
}

// DisableTracing just stops the data sources but doesn't free up any buffer.
// This is to allow the consumer to freeze the buffers (by stopping the trace)
// and then drain the buffers. The actual teardown of the TracingSession happens
// in FreeBuffers().
void TracingServiceImpl::DisableTracing(TracingSessionID tsid) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  TracingSession* tracing_session = GetTracingSession(tsid);
  if (!tracing_session) {
    // Can happen if the consumer calls this before EnableTracing() or after
    // FreeBuffers().
    PERFETTO_DLOG("DisableTracing() failed, invalid session ID %" PRIu64, tsid);
    return;
  }

  for (const auto& data_source_inst : tracing_session->data_source_instances) {
    const ProducerID producer_id = data_source_inst.first;
    const DataSourceInstanceID ds_inst_id = data_source_inst.second.instance_id;
    ProducerEndpointImpl* producer = GetProducer(producer_id);
    producer->TearDownDataSource(ds_inst_id);
  }
  tracing_session->data_source_instances.clear();

  // If the client requested us to periodically save the buffer into the passed
  // file, force a write pass.
  if (tracing_session->write_into_file) {
    tracing_session->write_period_ms = 0;
    ReadBuffers(tsid, nullptr);
  }

  if (tracing_session->tracing_enabled) {
    tracing_session->tracing_enabled = false;
    tracing_session->consumer->NotifyOnTracingDisabled();
  }

  // Deliberately NOT removing the session from |tracing_session_|, it's still
  // needed to call ReadBuffers(). FreeBuffers() will erase() the session.
}

void TracingServiceImpl::Flush(TracingSessionID tsid,
                               uint32_t timeout_ms,
                               ConsumerEndpoint::FlushCallback callback) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  TracingSession* tracing_session = GetTracingSession(tsid);
  if (!tracing_session) {
    PERFETTO_DLOG("Flush() failed, invalid session ID %" PRIu64, tsid);
    return;
  }

  if (tracing_session->pending_flushes.size() > 1000) {
    PERFETTO_ELOG("Too many flushes (%zu) pending for the tracing session",
                  tracing_session->pending_flushes.size());
    callback(false);
    return;
  }

  FlushRequestID flush_request_id = ++last_flush_request_id_;
  PendingFlush& pending_flush =
      tracing_session->pending_flushes
          .emplace_hint(tracing_session->pending_flushes.end(),
                        flush_request_id, PendingFlush(std::move(callback)))
          ->second;

  // Send a flush request to each producer involved in the tracing session. In
  // order to issue a flush request we have to build a map of all data source
  // instance ids enabled for each producer.
  std::map<ProducerID, std::vector<DataSourceInstanceID>> flush_map;
  for (const auto& data_source_inst : tracing_session->data_source_instances) {
    const ProducerID producer_id = data_source_inst.first;
    const DataSourceInstanceID ds_inst_id = data_source_inst.second.instance_id;
    flush_map[producer_id].push_back(ds_inst_id);
  }

  for (const auto& kv : flush_map) {
    ProducerID producer_id = kv.first;
    ProducerEndpointImpl* producer = GetProducer(producer_id);
    const std::vector<DataSourceInstanceID>& data_sources = kv.second;
    producer->Flush(flush_request_id, data_sources);
    pending_flush.producers.insert(producer_id);
  }

  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this, tsid, flush_request_id] {
        if (weak_this)
          weak_this->OnFlushTimeout(tsid, flush_request_id);
      },
      timeout_ms);
}

void TracingServiceImpl::NotifyFlushDoneForProducer(
    ProducerID producer_id,
    FlushRequestID flush_request_id) {
  for (auto& kv : tracing_sessions_) {
    // Remove all pending flushes <= |flush_request_id| for |producer_id|.
    auto& pending_flushes = kv.second.pending_flushes;
    auto end_it = pending_flushes.upper_bound(flush_request_id);
    for (auto it = pending_flushes.begin(); it != end_it;) {
      PendingFlush& pending_flush = it->second;
      pending_flush.producers.erase(producer_id);
      if (pending_flush.producers.empty()) {
        task_runner_->PostTask(
            std::bind(std::move(pending_flush.callback), /*success=*/true));
        it = pending_flushes.erase(it);
      } else {
        it++;
      }
    }  // for (pending_flushes)
  }    // for (tracing_session)
}

void TracingServiceImpl::OnFlushTimeout(TracingSessionID tsid,
                                        FlushRequestID flush_request_id) {
  TracingSession* tracing_session = GetTracingSession(tsid);
  if (!tracing_session)
    return;
  auto it = tracing_session->pending_flushes.find(flush_request_id);
  if (it == tracing_session->pending_flushes.end())
    return;  // Nominal case: flush was completed and acked on time.
  auto callback = std::move(it->second.callback);
  tracing_session->pending_flushes.erase(it);
  callback(/*success=*/false);
}

void TracingServiceImpl::FlushAndDisableTracing(TracingSessionID tsid) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  Flush(tsid, kFlushTimeoutMs, [weak_this, tsid](bool success) {
    PERFETTO_DLOG("Flush done (success: %d), disabling trace session %" PRIu64,
                  success, tsid);
    if (weak_this)
      weak_this->DisableTracing(tsid);
  });
}

// Note: when this is called to write into a file passed when starting tracing
// |consumer| will be == nullptr (as opposite to the case of a consumer asking
// to send the trace data back over IPC).
void TracingServiceImpl::ReadBuffers(TracingSessionID tsid,
                                     ConsumerEndpointImpl* consumer) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  TracingSession* tracing_session = GetTracingSession(tsid);
  if (!tracing_session) {
    // This will be hit systematically from the PostDelayedTask when directly
    // writing into the file (in which case consumer == nullptr). Suppress the
    // log in this case as it's just spam.
    if (consumer)
      PERFETTO_DLOG("Cannot ReadBuffers(): no tracing session is active");
    return;  // TODO(primiano): signal failure?
  }

  // This can happen if the file is closed by a previous task because it reaches
  // |max_file_size_bytes|.
  if (!tracing_session->write_into_file && !consumer)
    return;

  if (tracing_session->write_into_file && consumer) {
    // If the consumer enabled tracing and asked to save the contents into the
    // passed file makes little sense to also try to read the buffers over IPC,
    // as that would just steal data from the periodic draining task.
    PERFETTO_DCHECK(false);
    return;
  }

  std::vector<TracePacket> packets;
  packets.reserve(1024);  // Just an educated guess to avoid trivial expansions.
  MaybeSnapshotClocks(tracing_session, &packets);
  MaybeSnapshotStats(tracing_session, &packets);
  MaybeEmitTraceConfig(tracing_session, &packets);

  size_t packets_bytes = 0;  // SUM(slice.size() for each slice in |packets|).
  size_t total_slices = 0;   // SUM(#slices in |packets|).

  // Add up size for packets added by the Maybe* calls above.
  for (const TracePacket& packet : packets) {
    packets_bytes += packet.size();
    total_slices += packet.slices().size();
  }

  // This is a rough threshold to determine how much to read from the buffer in
  // each task. This is to avoid executing a single huge sending task for too
  // long and risk to hit the watchdog. This is *not* an upper bound: we just
  // stop accumulating new packets and PostTask *after* we cross this threshold.
  // This constant essentially balances the PostTask and IPC overhead vs the
  // responsiveness of the service. An extremely small value will cause one IPC
  // and one PostTask for each slice but will keep the service extremely
  // responsive. An extremely large value will batch the send for the full
  // buffer in one large task, will hit the blocking send() once the socket
  // buffers are full and hang the service for a bit (until the consumer
  // catches up).
  static constexpr size_t kApproxBytesPerTask = 32768;
  bool did_hit_threshold = false;

  // TODO(primiano): Extend the ReadBuffers API to allow reading only some
  // buffers, not all of them in one go.
  for (size_t buf_idx = 0;
       buf_idx < tracing_session->num_buffers() && !did_hit_threshold;
       buf_idx++) {
    auto tbuf_iter = buffers_.find(tracing_session->buffers_index[buf_idx]);
    if (tbuf_iter == buffers_.end()) {
      PERFETTO_DCHECK(false);
      continue;
    }
    TraceBuffer& tbuf = *tbuf_iter->second;
    tbuf.BeginRead();
    while (!did_hit_threshold) {
      TracePacket packet;
      uid_t producer_uid = kInvalidUid;
      if (!tbuf.ReadNextTracePacket(&packet, &producer_uid))
        break;
      PERFETTO_DCHECK(producer_uid != kInvalidUid);
      PERFETTO_DCHECK(packet.size() > 0);
      if (!PacketStreamValidator::Validate(packet.slices())) {
        PERFETTO_DLOG("Dropping invalid packet");
        continue;
      }

      // Append a slice with the trusted UID of the producer. This can't
      // be spoofed because above we validated that the existing slices
      // don't contain any trusted UID fields. For added safety we append
      // instead of prepending because according to protobuf semantics, if
      // the same field is encountered multiple times the last instance
      // takes priority. Note that truncated packets are also rejected, so
      // the producer can't give us a partial packet (e.g., a truncated
      // string) which only becomes valid when the UID is appended here.
      protos::TrustedPacket trusted_packet;
      trusted_packet.set_trusted_uid(static_cast<int32_t>(producer_uid));
      static constexpr size_t kTrustedBufSize = 16;
      Slice slice = Slice::Allocate(kTrustedBufSize);
      PERFETTO_CHECK(
          trusted_packet.SerializeToArray(slice.own_data(), kTrustedBufSize));
      slice.size = static_cast<size_t>(trusted_packet.GetCachedSize());
      PERFETTO_DCHECK(slice.size > 0 && slice.size <= kTrustedBufSize);
      packet.AddSlice(std::move(slice));

      // Append the packet (inclusive of the trusted uid) to |packets|.
      packets_bytes += packet.size();
      total_slices += packet.slices().size();
      did_hit_threshold = packets_bytes >= kApproxBytesPerTask &&
                          !tracing_session->write_into_file;
      packets.emplace_back(std::move(packet));
    }  // for(packets...)
  }    // for(buffers...)

  // If the caller asked us to write into a file by setting
  // |write_into_file| == true in the trace config, drain the packets read
  // (if any) into the given file descriptor.
  if (tracing_session->write_into_file) {
    const uint64_t max_size = tracing_session->max_file_size_bytes
                                  ? tracing_session->max_file_size_bytes
                                  : std::numeric_limits<size_t>::max();

    // When writing into a file, the file should look like a root trace.proto
    // message. Each packet should be prepended with a proto preamble stating
    // its field id (within trace.proto) and size. Hence the addition below.
    const size_t max_iovecs = total_slices + packets.size();

    size_t num_iovecs = 0;
    bool stop_writing_into_file = tracing_session->write_period_ms == 0;
    std::unique_ptr<struct iovec[]> iovecs(new struct iovec[max_iovecs]);
    size_t num_iovecs_at_last_packet = 0;
    uint64_t bytes_about_to_be_written = 0;
    for (TracePacket& packet : packets) {
      std::tie(iovecs[num_iovecs].iov_base, iovecs[num_iovecs].iov_len) =
          packet.GetProtoPreamble();
      bytes_about_to_be_written += iovecs[num_iovecs].iov_len;
      num_iovecs++;
      for (const Slice& slice : packet.slices()) {
        // writev() doesn't change the passed pointer. However, struct iovec
        // take a non-const ptr because it's the same struct used by readv().
        // Hence the const_cast here.
        char* start = static_cast<char*>(const_cast<void*>(slice.start));
        bytes_about_to_be_written += slice.size;
        iovecs[num_iovecs++] = {start, slice.size};
      }

      if (tracing_session->bytes_written_into_file +
              bytes_about_to_be_written >=
          max_size) {
        stop_writing_into_file = true;
        num_iovecs = num_iovecs_at_last_packet;
        break;
      }

      num_iovecs_at_last_packet = num_iovecs;
    }
    PERFETTO_DCHECK(num_iovecs <= max_iovecs);
    int fd = *tracing_session->write_into_file;

    uint64_t total_wr_size = 0;

    // writev() can take at most IOV_MAX entries per call. Batch them.
    constexpr size_t kIOVMax = IOV_MAX;
    for (size_t i = 0; i < num_iovecs; i += kIOVMax) {
      int iov_batch_size = static_cast<int>(std::min(num_iovecs - i, kIOVMax));
      ssize_t wr_size = PERFETTO_EINTR(writev(fd, &iovecs[i], iov_batch_size));
      if (wr_size <= 0) {
        PERFETTO_PLOG("writev() failed");
        stop_writing_into_file = true;
        break;
      }
      total_wr_size += static_cast<size_t>(wr_size);
    }

    tracing_session->bytes_written_into_file += total_wr_size;

    PERFETTO_DLOG("Draining into file, written: %" PRIu64 " KB, stop: %d",
                  (total_wr_size + 1023) / 1024, stop_writing_into_file);
    if (stop_writing_into_file) {
      tracing_session->write_into_file.reset();
      tracing_session->write_period_ms = 0;
      DisableTracing(tsid);
      return;
    }

    auto weak_this = weak_ptr_factory_.GetWeakPtr();
    task_runner_->PostDelayedTask(
        [weak_this, tsid] {
          if (weak_this)
            weak_this->ReadBuffers(tsid, nullptr);
        },
        tracing_session->delay_to_next_write_period_ms());
    return;
  }  // if (tracing_session->write_into_file)

  const bool has_more = did_hit_threshold;
  if (has_more) {
    auto weak_consumer = consumer->GetWeakPtr();
    auto weak_this = weak_ptr_factory_.GetWeakPtr();
    task_runner_->PostTask([weak_this, weak_consumer, tsid] {
      if (!weak_this || !weak_consumer)
        return;
      weak_this->ReadBuffers(tsid, weak_consumer.get());
    });
  }

  // Keep this as tail call, just in case the consumer re-enters.
  consumer->consumer_->OnTraceData(std::move(packets), has_more);
}

void TracingServiceImpl::FreeBuffers(TracingSessionID tsid) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Freeing buffers for session %" PRIu64, tsid);
  TracingSession* tracing_session = GetTracingSession(tsid);
  if (!tracing_session) {
    PERFETTO_DLOG("FreeBuffers() failed, invalid session ID %" PRIu64, tsid);
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

void TracingServiceImpl::RegisterDataSource(ProducerID producer_id,
                                            const DataSourceDescriptor& desc) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Producer %" PRIu16 " registered data source \"%s\"",
                producer_id, desc.name().c_str());

  PERFETTO_DCHECK(!desc.name().empty());
  auto reg_ds = data_sources_.emplace(desc.name(),
                                      RegisteredDataSource{producer_id, desc});

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
    TraceConfig::ProducerConfig producer_config;
    for (auto& config : tracing_session.config.producers()) {
      if (producer->name_ == config.producer_name()) {
        producer_config = config;
        break;
      }
    }
    for (const TraceConfig::DataSource& cfg_data_source :
         tracing_session.config.data_sources()) {
      if (cfg_data_source.config().name() == desc.name())
        CreateDataSourceInstance(cfg_data_source, producer_config,
                                 reg_ds->second, &tracing_session);
    }
  }
}

void TracingServiceImpl::UnregisterDataSource(ProducerID producer_id,
                                              const std::string& name) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_CHECK(producer_id);
  ProducerEndpointImpl* producer = GetProducer(producer_id);
  PERFETTO_DCHECK(producer);
  for (auto& kv : tracing_sessions_) {
    auto& ds_instances = kv.second.data_source_instances;
    for (auto it = ds_instances.begin(); it != ds_instances.end();) {
      if (it->first == producer_id && it->second.data_source_name == name) {
        DataSourceInstanceID ds_inst_id = it->second.instance_id;
        producer->TearDownDataSource(ds_inst_id);
        it = ds_instances.erase(it);
      } else {
        ++it;
      }
    }  // for (data_source_instances)
  }    // for (tracing_session)

  for (auto it = data_sources_.begin(); it != data_sources_.end(); ++it) {
    if (it->second.producer_id == producer_id &&
        it->second.descriptor.name() == name) {
      data_sources_.erase(it);
      return;
    }
  }

  PERFETTO_DLOG(
      "Tried to unregister a non-existent data source \"%s\" for "
      "producer %" PRIu16,
      name.c_str(), producer_id);
  PERFETTO_DCHECK(false);
}

void TracingServiceImpl::CreateDataSourceInstance(
    const TraceConfig::DataSource& cfg_data_source,
    const TraceConfig::ProducerConfig& producer_config,
    const RegisteredDataSource& data_source,
    TracingSession* tracing_session) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  ProducerEndpointImpl* producer = GetProducer(data_source.producer_id);
  PERFETTO_DCHECK(producer);
  // An existing producer that is not ftrace could have registered itself as
  // ftrace, we must not enable it in that case.
  if (lockdown_mode_ && producer->uid_ != uid_) {
    PERFETTO_DLOG("Lockdown mode: not enabling producer %hu", producer->id_);
    return;
  }
  // TODO(primiano): Add tests for registration ordering
  // (data sources vs consumers).
  if (!cfg_data_source.producer_name_filter().empty()) {
    if (std::find(cfg_data_source.producer_name_filter().begin(),
                  cfg_data_source.producer_name_filter().end(),
                  producer->name_) ==
        cfg_data_source.producer_name_filter().end()) {
      PERFETTO_DLOG("Data source: %s is filtered out for producer: %s",
                    cfg_data_source.config().name().c_str(),
                    producer->name_.c_str());
      return;
    }
  }

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
        "The TraceConfig for DataSource %s specified a target_buffer out of "
        "bound (%d). Skipping it.",
        ds_config.name().c_str(), relative_buffer_id);
    return;
  }
  BufferID global_id = tracing_session->buffers_index[relative_buffer_id];
  PERFETTO_DCHECK(global_id);
  ds_config.set_target_buffer(global_id);

  DataSourceInstanceID inst_id = ++last_data_source_instance_id_;
  tracing_session->data_source_instances.emplace(
      producer->id_,
      DataSourceInstance{inst_id, data_source.descriptor.name()});
  PERFETTO_DLOG("Starting data source %s with target buffer %" PRIu16,
                ds_config.name().c_str(), global_id);
  if (!producer->shared_memory()) {
    // Determine the SMB page size. Must be an integer multiple of 4k.
    size_t page_size = std::min<size_t>(producer_config.page_size_kb() * 1024,
                                        SharedMemoryABI::kMaxPageSize);
    if (page_size < base::kPageSize || page_size % base::kPageSize != 0)
      page_size = kDefaultShmPageSize;
    producer->shared_buffer_page_size_kb_ = page_size / 1024;

    // Determine the SMB size. Must be an integer multiple of the SMB page size.
    // The decisional tree is as follows:
    // 1. Give priority to what defined in the trace config.
    // 2. If unset give priority to the hint passed by the producer.
    // 3. Keep within bounds and ensure it's a multiple of the page size.
    size_t shm_size = producer_config.shm_size_kb() * 1024;
    if (shm_size == 0)
      shm_size = producer->shmem_size_hint_bytes_;
    shm_size = std::min<size_t>(shm_size, kMaxShmSize);
    if (shm_size < page_size || shm_size % page_size)
      shm_size = kDefaultShmSize;

    // TODO(primiano): right now Create() will suicide in case of OOM if the
    // mmap fails. We should instead gracefully fail the request and tell the
    // client to go away.
    auto shared_memory = shm_factory_->CreateSharedMemory(shm_size);
    producer->SetSharedMemory(std::move(shared_memory));
    producer->OnTracingSetup();
    UpdateMemoryGuardrail();
  }
  producer->CreateDataSourceInstance(inst_id, ds_config);
}

// Note: all the fields % *_trusted ones are untrusted, as in, the Producer
// might be lying / returning garbage contents. |src| and |size| can be trusted
// in terms of being a valid pointer, but not the contents.
void TracingServiceImpl::CopyProducerPageIntoLogBuffer(
    ProducerID producer_id_trusted,
    uid_t producer_uid_trusted,
    WriterID writer_id,
    ChunkID chunk_id,
    BufferID buffer_id,
    uint16_t num_fragments,
    uint8_t chunk_flags,
    const uint8_t* src,
    size_t size) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  TraceBuffer* buf = GetBufferByID(buffer_id);
  if (!buf) {
    PERFETTO_DLOG("Could not find target buffer %" PRIu16
                  " for producer %" PRIu16,
                  buffer_id, producer_id_trusted);
    return;
  }

  // TODO(primiano): we should have a set<BufferID> |allowed_target_buffers| in
  // ProducerEndpointImpl to perform ACL checks and prevent that the Producer
  // passes a |target_buffer| which is valid, but that we never asked it to use.
  // Essentially we want to prevent a malicious producer to inject data into a
  // log buffer that has nothing to do with it.

  buf->CopyChunkUntrusted(producer_id_trusted, producer_uid_trusted, writer_id,
                          chunk_id, num_fragments, chunk_flags, src, size);
}

void TracingServiceImpl::ApplyChunkPatches(
    ProducerID producer_id_trusted,
    const std::vector<CommitDataRequest::ChunkToPatch>& chunks_to_patch) {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  for (const auto& chunk : chunks_to_patch) {
    const ChunkID chunk_id = static_cast<ChunkID>(chunk.chunk_id());
    const WriterID writer_id = static_cast<WriterID>(chunk.writer_id());
    TraceBuffer* buf =
        GetBufferByID(static_cast<BufferID>(chunk.target_buffer()));
    static_assert(std::numeric_limits<ChunkID>::max() == kMaxChunkID,
                  "Add a '|| chunk_id > kMaxChunkID' below if this fails");
    if (!writer_id || writer_id > kMaxWriterID || !buf) {
      PERFETTO_DLOG(
          "Received invalid chunks_to_patch request from Producer: %" PRIu16
          ", BufferID: %" PRIu32 " ChunkdID: %" PRIu32 " WriterID: %" PRIu16,
          producer_id_trusted, chunk.target_buffer(), chunk_id, writer_id);
      continue;
    }
    // Speculate on the fact that there are going to be a limited amount of
    // patches per request, so we can allocate the |patches| array on the stack.
    std::array<TraceBuffer::Patch, 1024> patches;  // Uninitialized.
    if (chunk.patches().size() > patches.size()) {
      PERFETTO_DLOG("Too many patches (%zu) batched in the same request",
                    patches.size());
      PERFETTO_DCHECK(false);
      continue;
    }

    size_t i = 0;
    for (const auto& patch : chunk.patches()) {
      const std::string& patch_data = patch.data();
      if (patch_data.size() != patches[i].data.size()) {
        PERFETTO_DLOG("Received patch from producer: %" PRIu16
                      " of unexpected size %zu",
                      producer_id_trusted, patch_data.size());
        continue;
      }
      patches[i].offset_untrusted = patch.offset();
      memcpy(&patches[i].data[0], patch_data.data(), patches[i].data.size());
      i++;
    }
    buf->TryPatchChunkContents(producer_id_trusted, writer_id, chunk_id,
                               &patches[0], i, chunk.has_more_patches());
  }
}

TracingServiceImpl::TracingSession* TracingServiceImpl::GetTracingSession(
    TracingSessionID tsid) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto it = tsid ? tracing_sessions_.find(tsid) : tracing_sessions_.end();
  if (it == tracing_sessions_.end())
    return nullptr;
  return &it->second;
}

ProducerID TracingServiceImpl::GetNextProducerID() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_CHECK(producers_.size() < kMaxProducerID);
  do {
    ++last_producer_id_;
  } while (producers_.count(last_producer_id_) || last_producer_id_ == 0);
  PERFETTO_DCHECK(last_producer_id_ > 0 && last_producer_id_ <= kMaxProducerID);
  return last_producer_id_;
}

TraceBuffer* TracingServiceImpl::GetBufferByID(BufferID buffer_id) {
  auto buf_iter = buffers_.find(buffer_id);
  if (buf_iter == buffers_.end())
    return nullptr;
  return &*buf_iter->second;
}

void TracingServiceImpl::UpdateMemoryGuardrail() {
#if !PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
  uint64_t total_buffer_bytes = 0;

  // Sum up all the shared memory buffers.
  for (const auto& id_to_producer : producers_) {
    if (id_to_producer.second->shared_memory())
      total_buffer_bytes += id_to_producer.second->shared_memory()->size();
  }

  // Sum up all the trace buffers.
  for (const auto& id_to_buffer : buffers_) {
    total_buffer_bytes += id_to_buffer.second->size();
  }

  // Set the guard rail to 32MB + the sum of all the buffers over a 30 second
  // interval.
  uint64_t guardrail = 32 * 1024 * 1024 + total_buffer_bytes;
  base::Watchdog::GetInstance()->SetMemoryLimit(guardrail, 30 * 1000);
#endif
}

void TracingServiceImpl::MaybeSnapshotClocks(
    TracingSession* tracing_session,
    std::vector<TracePacket>* packets) {
  base::TimeMillis now = base::GetWallTimeMs();
  if (now < tracing_session->last_clock_snapshot + kClockSnapshotInterval)
    return;
  tracing_session->last_clock_snapshot = now;

  protos::TrustedPacket packet;
  protos::ClockSnapshot* clock_snapshot = packet.mutable_clock_snapshot();

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  struct {
    clockid_t id;
    protos::ClockSnapshot::Clock::Type type;
    struct timespec ts;
  } clocks[] = {
      {CLOCK_BOOTTIME, protos::ClockSnapshot::Clock::BOOTTIME, {0, 0}},
      {CLOCK_REALTIME_COARSE,
       protos::ClockSnapshot::Clock::REALTIME_COARSE,
       {0, 0}},
      {CLOCK_MONOTONIC_COARSE,
       protos::ClockSnapshot::Clock::MONOTONIC_COARSE,
       {0, 0}},
      {CLOCK_REALTIME, protos::ClockSnapshot::Clock::REALTIME, {0, 0}},
      {CLOCK_MONOTONIC, protos::ClockSnapshot::Clock::MONOTONIC, {0, 0}},
      {CLOCK_MONOTONIC_RAW,
       protos::ClockSnapshot::Clock::MONOTONIC_RAW,
       {0, 0}},
      {CLOCK_PROCESS_CPUTIME_ID,
       protos::ClockSnapshot::Clock::PROCESS_CPUTIME,
       {0, 0}},
      {CLOCK_THREAD_CPUTIME_ID,
       protos::ClockSnapshot::Clock::THREAD_CPUTIME,
       {0, 0}},
  };
  // First snapshot all the clocks as atomically as we can.
  for (auto& clock : clocks) {
    if (clock_gettime(clock.id, &clock.ts) == -1)
      PERFETTO_DLOG("clock_gettime failed for clock %d", clock.id);
  }
  for (auto& clock : clocks) {
    protos::ClockSnapshot::Clock* c = clock_snapshot->add_clocks();
    c->set_type(clock.type);
    c->set_timestamp(
        static_cast<uint64_t>(base::FromPosixTimespec(clock.ts).count()));
  }
#else   // !PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
  protos::ClockSnapshot::Clock* c = clock_snapshot->add_clocks();
  c->set_type(protos::ClockSnapshot::Clock::MONOTONIC);
  c->set_timestamp(static_cast<uint64_t>(base::GetWallTimeNs().count()));
#endif  // !PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)

  packet.set_trusted_uid(static_cast<int32_t>(uid_));
  Slice slice = Slice::Allocate(static_cast<size_t>(packet.ByteSize()));
  PERFETTO_CHECK(packet.SerializeWithCachedSizesToArray(slice.own_data()));
  packets->emplace_back();
  packets->back().AddSlice(std::move(slice));
}

void TracingServiceImpl::MaybeSnapshotStats(TracingSession* tracing_session,
                                            std::vector<TracePacket>* packets) {
  base::TimeMillis now = base::GetWallTimeMs();
  if (now < tracing_session->last_stats_snapshot + kStatsSnapshotInterval)
    return;
  tracing_session->last_stats_snapshot = now;

  protos::TrustedPacket packet;
  packet.set_trusted_uid(static_cast<int32_t>(uid_));

  protos::TraceStats* trace_stats = packet.mutable_trace_stats();
  trace_stats->set_producers_connected(
      static_cast<uint32_t>(producers_.size()));
  trace_stats->set_producers_seen(last_producer_id_);
  trace_stats->set_data_sources_registered(
      static_cast<uint32_t>(data_sources_.size()));
  trace_stats->set_data_sources_seen(last_data_source_instance_id_);
  trace_stats->set_tracing_sessions(
      static_cast<uint32_t>(tracing_sessions_.size()));
  trace_stats->set_total_buffers(static_cast<uint32_t>(buffers_.size()));

  for (BufferID buf_id : tracing_session->buffers_index) {
    TraceBuffer* buf = GetBufferByID(buf_id);
    if (!buf) {
      PERFETTO_DCHECK(false);
      continue;
    }
    auto* buf_stats_proto = trace_stats->add_buffer_stats();
    const TraceBuffer::Stats& buf_stats = buf->stats();
    buf_stats_proto->set_bytes_written(buf_stats.bytes_written);
    buf_stats_proto->set_chunks_written(buf_stats.chunks_written);
    buf_stats_proto->set_chunks_overwritten(buf_stats.chunks_overwritten);
    buf_stats_proto->set_write_wrap_count(buf_stats.write_wrap_count);
    buf_stats_proto->set_patches_succeeded(buf_stats.patches_succeeded);
    buf_stats_proto->set_patches_failed(buf_stats.patches_failed);
    buf_stats_proto->set_readaheads_succeeded(buf_stats.readaheads_succeeded);
    buf_stats_proto->set_readaheads_failed(buf_stats.readaheads_failed);
    buf_stats_proto->set_abi_violations(buf_stats.abi_violations);
  }  // for (buf in session).
  Slice slice = Slice::Allocate(static_cast<size_t>(packet.ByteSize()));
  PERFETTO_CHECK(packet.SerializeWithCachedSizesToArray(slice.own_data()));
  packets->emplace_back();
  packets->back().AddSlice(std::move(slice));
}

void TracingServiceImpl::MaybeEmitTraceConfig(
    TracingSession* tracing_session,
    std::vector<TracePacket>* packets) {
  if (tracing_session->did_emit_config)
    return;
  tracing_session->did_emit_config = true;
  protos::TrustedPacket packet;
  tracing_session->config.ToProto(packet.mutable_trace_config());
  packet.set_trusted_uid(static_cast<int32_t>(uid_));
  Slice slice = Slice::Allocate(static_cast<size_t>(packet.ByteSize()));
  PERFETTO_CHECK(packet.SerializeWithCachedSizesToArray(slice.own_data()));
  packets->emplace_back();
  packets->back().AddSlice(std::move(slice));
}

////////////////////////////////////////////////////////////////////////////////
// TracingServiceImpl::ConsumerEndpointImpl implementation
////////////////////////////////////////////////////////////////////////////////

TracingServiceImpl::ConsumerEndpointImpl::ConsumerEndpointImpl(
    TracingServiceImpl* service,
    base::TaskRunner* task_runner,
    Consumer* consumer)
    : task_runner_(task_runner),
      service_(service),
      consumer_(consumer),
      weak_ptr_factory_(this) {}

TracingServiceImpl::ConsumerEndpointImpl::~ConsumerEndpointImpl() {
  service_->DisconnectConsumer(this);
  consumer_->OnDisconnect();
}

void TracingServiceImpl::ConsumerEndpointImpl::NotifyOnTracingDisabled() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto weak_this = GetWeakPtr();
  task_runner_->PostTask([weak_this] {
    if (weak_this)
      weak_this->consumer_->OnTracingDisabled();
  });
}

void TracingServiceImpl::ConsumerEndpointImpl::EnableTracing(
    const TraceConfig& cfg,
    base::ScopedFile fd) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!service_->EnableTracing(this, cfg, std::move(fd)))
    NotifyOnTracingDisabled();
}

void TracingServiceImpl::ConsumerEndpointImpl::DisableTracing() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!tracing_session_id_) {
    PERFETTO_LOG("Consumer called DisableTracing() but tracing was not active");
    return;
  }
  service_->DisableTracing(tracing_session_id_);
}

void TracingServiceImpl::ConsumerEndpointImpl::ReadBuffers() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!tracing_session_id_) {
    PERFETTO_LOG("Consumer called ReadBuffers() but tracing was not active");
    return;
  }
  service_->ReadBuffers(tracing_session_id_, this);
}

void TracingServiceImpl::ConsumerEndpointImpl::FreeBuffers() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!tracing_session_id_) {
    PERFETTO_LOG("Consumer called FreeBuffers() but tracing was not active");
    return;
  }
  service_->FreeBuffers(tracing_session_id_);
  tracing_session_id_ = 0;
}

void TracingServiceImpl::ConsumerEndpointImpl::Flush(uint32_t timeout_ms,
                                                     FlushCallback callback) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!tracing_session_id_) {
    PERFETTO_LOG("Consumer called Flush() but tracing was not active");
    return;
  }
  service_->Flush(tracing_session_id_, timeout_ms, callback);
}

base::WeakPtr<TracingServiceImpl::ConsumerEndpointImpl>
TracingServiceImpl::ConsumerEndpointImpl::GetWeakPtr() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  return weak_ptr_factory_.GetWeakPtr();
}

////////////////////////////////////////////////////////////////////////////////
// TracingServiceImpl::ProducerEndpointImpl implementation
////////////////////////////////////////////////////////////////////////////////

TracingServiceImpl::ProducerEndpointImpl::ProducerEndpointImpl(
    ProducerID id,
    uid_t uid,
    TracingServiceImpl* service,
    base::TaskRunner* task_runner,
    Producer* producer,
    const std::string& producer_name)
    : id_(id),
      uid_(uid),
      service_(service),
      task_runner_(task_runner),
      producer_(producer),
      name_(producer_name),
      weak_ptr_factory_(this) {}

TracingServiceImpl::ProducerEndpointImpl::~ProducerEndpointImpl() {
  service_->DisconnectProducer(id_);
  producer_->OnDisconnect();
}

void TracingServiceImpl::ProducerEndpointImpl::RegisterDataSource(
    const DataSourceDescriptor& desc) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (desc.name().empty()) {
    PERFETTO_DLOG("Received RegisterDataSource() with empty name");
    return;
  }

  service_->RegisterDataSource(id_, desc);
}

void TracingServiceImpl::ProducerEndpointImpl::UnregisterDataSource(
    const std::string& name) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  service_->UnregisterDataSource(id_, name);
}

void TracingServiceImpl::ProducerEndpointImpl::CommitData(
    const CommitDataRequest& req_untrusted,
    CommitDataCallback callback) {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  if (!shared_memory_) {
    PERFETTO_DLOG(
        "Attempted to commit data before the shared memory was allocated.");
    return;
  }
  PERFETTO_DCHECK(shmem_abi_.is_valid());
  for (const auto& entry : req_untrusted.chunks_to_move()) {
    const uint32_t page_idx = entry.page();
    if (page_idx >= shmem_abi_.num_pages())
      continue;  // A buggy or malicious producer.

    SharedMemoryABI::Chunk chunk =
        shmem_abi_.TryAcquireChunkForReading(page_idx, entry.chunk());
    if (!chunk.is_valid()) {
      PERFETTO_DLOG("Asked to move chunk %d:%d, but it's not complete",
                    entry.page(), entry.chunk());
      continue;
    }

    // TryAcquireChunkForReading() has load-acquire semantics. Once acquired,
    // the ABI contract expects the producer to not touch the chunk anymore
    // (until the service marks that as free). This is why all the reads below
    // are just memory_order_relaxed. Also, the code here assumes that all this
    // data can be malicious and just gives up if anything is malformed.
    BufferID buffer_id = static_cast<BufferID>(entry.target_buffer());
    const SharedMemoryABI::ChunkHeader& chunk_header = *chunk.header();
    WriterID writer_id = chunk_header.writer_id.load(std::memory_order_relaxed);
    ChunkID chunk_id = chunk_header.chunk_id.load(std::memory_order_relaxed);
    auto packets = chunk_header.packets.load(std::memory_order_relaxed);
    uint16_t num_fragments = packets.count;
    uint8_t chunk_flags = packets.flags;

    service_->CopyProducerPageIntoLogBuffer(
        id_, uid_, writer_id, chunk_id, buffer_id, num_fragments, chunk_flags,
        chunk.payload_begin(), chunk.payload_size());

    // This one has release-store semantics.
    shmem_abi_.ReleaseChunkAsFree(std::move(chunk));
  }  // for(chunks_to_move)

  service_->ApplyChunkPatches(id_, req_untrusted.chunks_to_patch());

  if (req_untrusted.flush_request_id()) {
    service_->NotifyFlushDoneForProducer(id_, req_untrusted.flush_request_id());
  }

  // Keep this invocation last. ProducerIPCService::CommitData() relies on this
  // callback being invoked within the same callstack and not posted. If this
  // changes, the code there needs to be changed accordingly.
  if (callback)
    callback();
}

void TracingServiceImpl::ProducerEndpointImpl::SetSharedMemory(
    std::unique_ptr<SharedMemory> shared_memory) {
  PERFETTO_DCHECK(!shared_memory_ && !shmem_abi_.is_valid());
  shared_memory_ = std::move(shared_memory);
  shmem_abi_.Initialize(reinterpret_cast<uint8_t*>(shared_memory_->start()),
                        shared_memory_->size(),
                        shared_buffer_page_size_kb() * 1024);
}

SharedMemory* TracingServiceImpl::ProducerEndpointImpl::shared_memory() const {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  return shared_memory_.get();
}

size_t TracingServiceImpl::ProducerEndpointImpl::shared_buffer_page_size_kb()
    const {
  return shared_buffer_page_size_kb_;
}

void TracingServiceImpl::ProducerEndpointImpl::TearDownDataSource(
    DataSourceInstanceID ds_inst_id) {
  // TODO(primiano): When we'll support tearing down the SMB, at this point we
  // should send the Producer a TearDownTracing if all its data sources have
  // been disabled (see b/77532839 and aosp/655179 PS1).
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_this, ds_inst_id] {
    if (weak_this)
      weak_this->producer_->TearDownDataSourceInstance(ds_inst_id);
  });
}

SharedMemoryArbiterImpl*
TracingServiceImpl::ProducerEndpointImpl::GetOrCreateShmemArbiter() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!inproc_shmem_arbiter_) {
    inproc_shmem_arbiter_.reset(new SharedMemoryArbiterImpl(
        shared_memory_->start(), shared_memory_->size(),
        shared_buffer_page_size_kb_ * 1024, this, task_runner_));
  }
  return inproc_shmem_arbiter_.get();
}

std::unique_ptr<TraceWriter>
TracingServiceImpl::ProducerEndpointImpl::CreateTraceWriter(BufferID buf_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  return GetOrCreateShmemArbiter()->CreateTraceWriter(buf_id);
}

void TracingServiceImpl::ProducerEndpointImpl::OnTracingSetup() {
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_this] {
    if (weak_this)
      weak_this->producer_->OnTracingSetup();
  });
}

void TracingServiceImpl::ProducerEndpointImpl::Flush(
    FlushRequestID flush_request_id,
    const std::vector<DataSourceInstanceID>& data_sources) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_this, flush_request_id, data_sources] {
    if (weak_this) {
      weak_this->producer_->Flush(flush_request_id, data_sources.data(),
                                  data_sources.size());
    }
  });
}

void TracingServiceImpl::ProducerEndpointImpl::CreateDataSourceInstance(
    DataSourceInstanceID ds_id,
    const DataSourceConfig& config) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_this, ds_id, config] {
    if (weak_this)
      weak_this->producer_->CreateDataSourceInstance(ds_id, std::move(config));
  });
}

void TracingServiceImpl::ProducerEndpointImpl::NotifyFlushComplete(
    FlushRequestID id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  return GetOrCreateShmemArbiter()->NotifyFlushComplete(id);
}

////////////////////////////////////////////////////////////////////////////////
// TracingServiceImpl::TracingSession implementation
////////////////////////////////////////////////////////////////////////////////

TracingServiceImpl::TracingSession::TracingSession(
    ConsumerEndpointImpl* consumer_ptr,
    const TraceConfig& new_config)
    : consumer(consumer_ptr), config(new_config) {}

}  // namespace perfetto
