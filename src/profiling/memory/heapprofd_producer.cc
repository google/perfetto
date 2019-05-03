/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/profiling/memory/heapprofd_producer.h"

#include <inttypes.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "perfetto/base/file_utils.h"
#include "perfetto/base/string_utils.h"
#include "perfetto/base/thread_task_runner.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "perfetto/tracing/ipc/producer_ipc_client.h"

namespace perfetto {
namespace profiling {
namespace {
using ::perfetto::protos::pbzero::ProfilePacket;

constexpr char kHeapprofdDataSource[] = "android.heapprofd";
constexpr size_t kUnwinderThreads = 5;
constexpr int kHeapprofdSignal = 36;

constexpr uint32_t kInitialConnectionBackoffMs = 100;
constexpr uint32_t kMaxConnectionBackoffMs = 30 * 1000;

constexpr uint32_t kChildModeWatchdogPeriodMs = 10 * 1000;

constexpr uint64_t kDefaultShmemSize = 8 * 1048576;  // ~8 MB
constexpr uint64_t kMaxShmemSize = 500 * 1048576;    // ~500 MB

ClientConfiguration MakeClientConfiguration(const DataSourceConfig& cfg) {
  ClientConfiguration client_config;
  client_config.interval = cfg.heapprofd_config().sampling_interval_bytes();
  client_config.block_client = cfg.heapprofd_config().block_client();
  return client_config;
}

std::vector<UnwindingWorker> MakeUnwindingWorkers(HeapprofdProducer* delegate,
                                                  size_t n) {
  std::vector<UnwindingWorker> ret;
  for (size_t i = 0; i < n; ++i) {
    ret.emplace_back(delegate, base::ThreadTaskRunner::CreateAndStart());
  }
  return ret;
}

bool ConfigTargetsProcess(const HeapprofdConfig& cfg,
                          const Process& proc,
                          const std::vector<std::string>& normalized_cmdlines) {
  if (cfg.all())
    return true;

  const auto& pids = cfg.pid();
  if (std::find(pids.cbegin(), pids.cend(), static_cast<uint64_t>(proc.pid)) !=
      pids.cend()) {
    return true;
  }

  if (std::find(normalized_cmdlines.cbegin(), normalized_cmdlines.cend(),
                proc.cmdline) != normalized_cmdlines.cend()) {
    return true;
  }
  return false;
}

// Return largest n such that pow(2, n) < value.
size_t Log2LessThan(uint64_t value) {
  size_t i = 0;
  while (value) {
    i++;
    value >>= 1;
  }
  return i;
}

}  // namespace

const uint64_t LogHistogram::kMaxBucket = 0;

std::vector<std::pair<uint64_t, uint64_t>> LogHistogram::GetData() {
  std::vector<std::pair<uint64_t, uint64_t>> data;
  data.reserve(kBuckets);
  for (size_t i = 0; i < kBuckets; ++i) {
    if (i == kBuckets - 1)
      data.emplace_back(kMaxBucket, values_[i]);
    else
      data.emplace_back(1 << i, values_[i]);
  }
  return data;
}

size_t LogHistogram::GetBucket(uint64_t value) {
  if (value == 0)
    return 0;

  size_t hibit = Log2LessThan(value);
  if (hibit >= kBuckets)
    return kBuckets - 1;
  return hibit;
}

// We create kUnwinderThreads unwinding threads. Bookkeeping is done on the main
// thread.
HeapprofdProducer::HeapprofdProducer(HeapprofdMode mode,
                                     base::TaskRunner* task_runner)
    : task_runner_(task_runner),
      mode_(mode),
      unwinding_workers_(MakeUnwindingWorkers(this, kUnwinderThreads)),
      socket_delegate_(this),
      weak_factory_(this) {
  if (mode == HeapprofdMode::kCentral) {
    listening_socket_ = MakeListeningSocket();
  }
}

HeapprofdProducer::~HeapprofdProducer() {
  // We only borrowed this from the environment variable.
  // UnixSocket always owns the socket, so we need to manually release it
  // here.
  if (mode_ == HeapprofdMode::kCentral && bool(listening_socket_))
    listening_socket_->ReleaseSocket().ReleaseFd().release();
}

std::unique_ptr<base::UnixSocket> HeapprofdProducer::MakeListeningSocket() {
  const char* sock_fd = getenv(kHeapprofdSocketEnvVar);
  if (sock_fd == nullptr) {
    unlink(kHeapprofdSocketFile);
    return base::UnixSocket::Listen(kHeapprofdSocketFile, &socket_delegate_,
                                    task_runner_);
  }
  char* end;
  int raw_fd = static_cast<int>(strtol(sock_fd, &end, 10));
  if (*end != '\0')
    PERFETTO_FATAL("Invalid %s. Expected decimal integer.",
                   kHeapprofdSocketEnvVar);
  return base::UnixSocket::Listen(base::ScopedFile(raw_fd), &socket_delegate_,
                                  task_runner_);
}

void HeapprofdProducer::SetTargetProcess(pid_t target_pid,
                                         std::string target_cmdline,
                                         base::ScopedFile inherited_socket) {
  target_process_.pid = target_pid;
  target_process_.cmdline = target_cmdline;
  inherited_fd_ = std::move(inherited_socket);
}

void HeapprofdProducer::AdoptTargetProcessSocket() {
  PERFETTO_DCHECK(mode_ == HeapprofdMode::kChild);
  auto socket = base::UnixSocket::AdoptConnected(
      std::move(inherited_fd_), &socket_delegate_, task_runner_,
      base::SockType::kStream);

  HandleClientConnection(std::move(socket), target_process_);
}

void HeapprofdProducer::OnConnect() {
  PERFETTO_DCHECK(state_ == kConnecting);
  state_ = kConnected;
  ResetConnectionBackoff();
  PERFETTO_LOG("Connected to the service, mode [%s].",
               mode_ == HeapprofdMode::kCentral ? "central" : "child");

  DataSourceDescriptor desc;
  desc.set_name(kHeapprofdDataSource);
  endpoint_->RegisterDataSource(desc);
}

void HeapprofdProducer::OnDisconnect() {
  PERFETTO_DCHECK(state_ == kConnected || state_ == kConnecting);
  PERFETTO_LOG("Disconnected from tracing service");

  // Do not attempt to reconnect if we're a process-private process, just quit.
  if (mode_ == HeapprofdMode::kChild) {
    TerminateProcess(/*exit_status=*/1);  // does not return
  }

  // Central mode - attempt to reconnect.
  auto weak_producer = weak_factory_.GetWeakPtr();
  if (state_ == kConnected)
    return task_runner_->PostTask([weak_producer] {
      if (!weak_producer)
        return;
      weak_producer->Restart();
    });

  state_ = kNotConnected;
  IncreaseConnectionBackoff();
  task_runner_->PostDelayedTask(
      [weak_producer] {
        if (!weak_producer)
          return;
        weak_producer->ConnectService();
      },
      connection_backoff_ms_);
}

void HeapprofdProducer::ConnectWithRetries(const char* socket_name) {
  PERFETTO_DCHECK(state_ == kNotStarted);
  state_ = kNotConnected;

  ResetConnectionBackoff();
  producer_sock_name_ = socket_name;
  ConnectService();
}

void HeapprofdProducer::ConnectService() {
  SetProducerEndpoint(ProducerIPCClient::Connect(
      producer_sock_name_, this, "android.heapprofd", task_runner_));
}

void HeapprofdProducer::SetProducerEndpoint(
    std::unique_ptr<TracingService::ProducerEndpoint> endpoint) {
  PERFETTO_DCHECK(state_ == kNotConnected || state_ == kNotStarted);
  state_ = kConnecting;
  endpoint_ = std::move(endpoint);
}

void HeapprofdProducer::IncreaseConnectionBackoff() {
  connection_backoff_ms_ *= 2;
  if (connection_backoff_ms_ > kMaxConnectionBackoffMs)
    connection_backoff_ms_ = kMaxConnectionBackoffMs;
}

void HeapprofdProducer::ResetConnectionBackoff() {
  connection_backoff_ms_ = kInitialConnectionBackoffMs;
}

void HeapprofdProducer::Restart() {
  // We lost the connection with the tracing service. At this point we need
  // to reset all the data sources. Trying to handle that manually is going to
  // be error prone. What we do here is simply destroy the instance and
  // recreate it again.

  // Child mode producer should not attempt restarts. Note that this also means
  // the rest of this method doesn't have to handle child-specific state.
  if (mode_ == HeapprofdMode::kChild)
    PERFETTO_FATAL("Attempting to restart a child mode producer.");

  HeapprofdMode mode = mode_;
  base::TaskRunner* task_runner = task_runner_;
  const char* socket_name = producer_sock_name_;

  // Invoke destructor and then the constructor again.
  this->~HeapprofdProducer();
  new (this) HeapprofdProducer(mode, task_runner);

  ConnectWithRetries(socket_name);
}

void HeapprofdProducer::ScheduleActiveDataSourceWatchdog() {
  PERFETTO_DCHECK(mode_ == HeapprofdMode::kChild);

  // Post the first check after a delay, to let the freshly forked heapprofd
  // to receive the active data sources from traced. The checks will reschedule
  // themselves from that point onwards.
  auto weak_producer = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_producer]() {
        if (!weak_producer)
          return;
        weak_producer->ActiveDataSourceWatchdogCheck();
      },
      kChildModeWatchdogPeriodMs);
}

void HeapprofdProducer::ActiveDataSourceWatchdogCheck() {
  PERFETTO_DCHECK(mode_ == HeapprofdMode::kChild);

  // Fork mode heapprofd should be working on exactly one data source matching
  // its target process.
  if (data_sources_.empty()) {
    PERFETTO_LOG(
        "Child heapprofd exiting as it never received a data source for the "
        "target process, or somehow lost/finished the task without exiting.");
    TerminateProcess(/*exit_status=*/1);
  } else {
    // reschedule check.
    auto weak_producer = weak_factory_.GetWeakPtr();
    task_runner_->PostDelayedTask(
        [weak_producer]() {
          if (!weak_producer)
            return;
          weak_producer->ActiveDataSourceWatchdogCheck();
        },
        kChildModeWatchdogPeriodMs);
  }
}

// TODO(rsavitski): would be cleaner to shut down the event loop instead
// (letting main exit). One test-friendly approach is to supply a shutdown
// callback in the constructor.
__attribute__((noreturn)) void HeapprofdProducer::TerminateProcess(
    int exit_status) {
  PERFETTO_CHECK(mode_ == HeapprofdMode::kChild);
  exit(exit_status);
}

void HeapprofdProducer::OnTracingSetup() {}

void HeapprofdProducer::SetupDataSource(DataSourceInstanceID id,
                                        const DataSourceConfig& cfg) {
  PERFETTO_DLOG("Setting up data source.");
  if (mode_ == HeapprofdMode::kChild && cfg.enable_extra_guardrails()) {
    PERFETTO_ELOG("enable_extra_guardrails is not supported on user.");
    return;
  }

  const HeapprofdConfig& heapprofd_config = cfg.heapprofd_config();
  if (heapprofd_config.all() && !heapprofd_config.pid().empty())
    PERFETTO_ELOG("No point setting all and pid");
  if (heapprofd_config.all() && !heapprofd_config.process_cmdline().empty())
    PERFETTO_ELOG("No point setting all and process_cmdline");

  if (cfg.name() != kHeapprofdDataSource) {
    PERFETTO_DLOG("Invalid data source name.");
    return;
  }

  auto it = data_sources_.find(id);
  if (it != data_sources_.end()) {
    PERFETTO_DFATAL("Received duplicated data source instance id: %" PRIu64,
                    id);
    return;
  }

  std::vector<std::string> normalized_cmdlines =
      NormalizeCmdlines(heapprofd_config.process_cmdline());

  // Child mode is only interested in the first data source matching the
  // already-connected process.
  if (mode_ == HeapprofdMode::kChild) {
    if (!ConfigTargetsProcess(heapprofd_config, target_process_,
                              normalized_cmdlines)) {
      PERFETTO_DLOG("Child mode skipping setup of unrelated data source.");
      return;
    }

    if (!data_sources_.empty()) {
      PERFETTO_LOG("Child mode skipping concurrent data source.");

      // Manually write one ProfilePacket about the rejected session.
      auto buffer_id = static_cast<BufferID>(cfg.target_buffer());
      auto trace_writer = endpoint_->CreateTraceWriter(buffer_id);
      auto trace_packet = trace_writer->NewTracePacket();
      auto profile_packet = trace_packet->set_profile_packet();
      auto process_dump = profile_packet->add_process_dumps();
      process_dump->set_pid(static_cast<uint64_t>(target_process_.pid));
      process_dump->set_rejected_concurrent(true);
      trace_packet->Finalize();
      trace_writer->Flush();
      return;
    }
  }

  DataSource data_source;
  data_source.id = id;
  data_source.client_configuration = MakeClientConfiguration(cfg);
  data_source.config = heapprofd_config;
  auto buffer_id = static_cast<BufferID>(cfg.target_buffer());
  data_source.trace_writer = endpoint_->CreateTraceWriter(buffer_id);
  data_source.normalized_cmdlines = std::move(normalized_cmdlines);

  data_sources_.emplace(id, std::move(data_source));
  PERFETTO_DLOG("Set up data source.");

  if (mode_ == HeapprofdMode::kChild)
    AdoptTargetProcessSocket();
}

bool HeapprofdProducer::IsPidProfiled(pid_t pid) {
  for (const auto& pair : data_sources_) {
    const DataSource& ds = pair.second;
    if (ds.process_states.find(pid) != ds.process_states.cend())
      return true;
  }
  return false;
}

void HeapprofdProducer::StartDataSource(DataSourceInstanceID id,
                                        const DataSourceConfig& cfg) {
  PERFETTO_DLOG("Start DataSource");
  const HeapprofdConfig& heapprofd_config = cfg.heapprofd_config();

  auto it = data_sources_.find(id);
  if (it == data_sources_.end()) {
    // This is expected in child heapprofd, where we reject uninteresting data
    // sources in SetupDataSource.
    if (mode_ == HeapprofdMode::kCentral) {
      PERFETTO_DFATAL(
          "Received invalid data source instance to start: %" PRIu64, id);
    }
    return;
  }
  DataSource& data_source = it->second;

  // Central daemon - set system properties for any targets that start later,
  // and signal already-running targets to start the profiling client.
  if (mode_ == HeapprofdMode::kCentral) {
    if (heapprofd_config.all())
      data_source.properties.emplace_back(properties_.SetAll());

    for (std::string cmdline : data_source.normalized_cmdlines)
      data_source.properties.emplace_back(
          properties_.SetProperty(std::move(cmdline)));

    std::set<pid_t> pids;
    if (heapprofd_config.all())
      FindAllProfilablePids(&pids);
    for (uint64_t pid : heapprofd_config.pid())
      pids.emplace(static_cast<pid_t>(pid));

    if (!data_source.normalized_cmdlines.empty())
      FindPidsForCmdlines(data_source.normalized_cmdlines, &pids);

    for (auto pid_it = pids.cbegin(); pid_it != pids.cend();) {
      pid_t pid = *pid_it;
      if (IsPidProfiled(pid)) {
        PERFETTO_LOG("Rejecting concurrent session for %" PRIdMAX,
                     static_cast<intmax_t>(pid));
        data_source.rejected_pids.emplace(pid);
        pid_it = pids.erase(pid_it);
        continue;
      }

      PERFETTO_DLOG("Sending %d to %d", kHeapprofdSignal, pid);
      if (kill(pid, kHeapprofdSignal) != 0) {
        PERFETTO_DPLOG("kill");
      }
      ++pid_it;
    }
    data_source.signaled_pids = std::move(pids);
  }

  const auto continuous_dump_config = heapprofd_config.continuous_dump_config();
  uint32_t dump_interval = continuous_dump_config.dump_interval_ms();
  if (dump_interval) {
    auto weak_producer = weak_factory_.GetWeakPtr();
    task_runner_->PostDelayedTask(
        [weak_producer, id, dump_interval] {
          if (!weak_producer)
            return;
          weak_producer->DoContinuousDump(id, dump_interval);
        },
        continuous_dump_config.dump_phase_ms());
  }
  PERFETTO_DLOG("Started DataSource");
}

UnwindingWorker& HeapprofdProducer::UnwinderForPID(pid_t pid) {
  return unwinding_workers_[static_cast<uint64_t>(pid) % kUnwinderThreads];
}

void HeapprofdProducer::StopDataSource(DataSourceInstanceID id) {
  auto it = data_sources_.find(id);
  if (it == data_sources_.end()) {
    if (mode_ == HeapprofdMode::kCentral)
      PERFETTO_DFATAL("Trying to stop non existing data source: %" PRIu64, id);
    return;
  }

  DataSource& data_source = it->second;
  for (const auto& pid_and_process_state : data_source.process_states) {
    pid_t pid = pid_and_process_state.first;
    UnwinderForPID(pid).PostDisconnectSocket(pid);
  }

  data_sources_.erase(it);

  if (mode_ == HeapprofdMode::kChild)
    TerminateProcess(/*exit_status=*/0);  // does not return
}

void HeapprofdProducer::DoContinuousDump(DataSourceInstanceID id,
                                         uint32_t dump_interval) {
  if (!Dump(id, 0 /* flush_id */, false /* is_flush */))
    return;
  auto weak_producer = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_producer, id, dump_interval] {
        if (!weak_producer)
          return;
        weak_producer->DoContinuousDump(id, dump_interval);
      },
      dump_interval);
}

bool HeapprofdProducer::Dump(DataSourceInstanceID id,
                             FlushRequestID flush_id,
                             bool has_flush_id) {
  auto it = data_sources_.find(id);
  if (it == data_sources_.end()) {
    PERFETTO_LOG(
        "Data source not found (harmless if using continuous_dump_config).");
    return false;
  }
  DataSource& data_source = it->second;

  DumpState dump_state(data_source.trace_writer.get(),
                       &data_source.next_index_);

  for (pid_t rejected_pid : data_source.rejected_pids) {
    ProfilePacket::ProcessHeapSamples* proto =
        dump_state.current_profile_packet->add_process_dumps();
    proto->set_pid(static_cast<uint64_t>(rejected_pid));
    proto->set_rejected_concurrent(true);
  }

  for (std::pair<const pid_t, ProcessState>& pid_and_process_state :
       data_source.process_states) {
    pid_t pid = pid_and_process_state.first;
    ProcessState& process_state = pid_and_process_state.second;
    HeapTracker& heap_tracker = process_state.heap_tracker;
    bool from_startup =
        data_source.signaled_pids.find(pid) == data_source.signaled_pids.cend();
    auto new_heapsamples = [pid, from_startup, &process_state](
                               ProfilePacket::ProcessHeapSamples* proto) {
      proto->set_pid(static_cast<uint64_t>(pid));
      proto->set_from_startup(from_startup);
      proto->set_disconnected(process_state.disconnected);
      proto->set_buffer_overran(process_state.buffer_overran);
      proto->set_buffer_corrupted(process_state.buffer_corrupted);
      auto* stats = proto->set_stats();
      stats->set_unwinding_errors(process_state.unwinding_errors);
      stats->set_heap_samples(process_state.heap_samples);
      stats->set_map_reparses(process_state.map_reparses);
      stats->set_total_unwinding_time_us(process_state.total_unwinding_time_us);
      auto* unwinding_hist = stats->set_unwinding_time_us();
      for (const auto& p : process_state.unwinding_time_us.GetData()) {
        auto* bucket = unwinding_hist->add_buckets();
        if (p.first == LogHistogram::kMaxBucket)
          bucket->set_max_bucket(true);
        else
          bucket->set_upper_limit(p.first);
        bucket->set_count(p.second);
      }
    };
    heap_tracker.Dump(std::move(new_heapsamples), &dump_state);
  }

  for (GlobalCallstackTrie::Node* node : dump_state.callstacks_to_dump) {
    // There need to be two separate loops over built_callstack because
    // protozero cannot interleave different messages.
    auto built_callstack = callsites_.BuildCallstack(node);
    for (const Interned<Frame>& frame : built_callstack)
      dump_state.WriteFrame(frame);
    ProfilePacket::Callstack* callstack =
        dump_state.current_profile_packet->add_callstacks();
    callstack->set_id(node->id());
    for (const Interned<Frame>& frame : built_callstack)
      callstack->add_frame_ids(frame.id());
  }

  dump_state.current_trace_packet->Finalize();
  if (has_flush_id) {
    auto weak_producer = weak_factory_.GetWeakPtr();
    auto callback = [weak_producer, flush_id] {
      if (weak_producer)
        return weak_producer->task_runner_->PostTask([weak_producer, flush_id] {
          if (weak_producer)
            return weak_producer->FinishDataSourceFlush(flush_id);
        });
    };
    data_source.trace_writer->Flush(std::move(callback));
  }
  return true;
}

void HeapprofdProducer::DumpAll() {
  for (const auto& id_and_data_source : data_sources_) {
    if (!Dump(id_and_data_source.first, 0 /* flush_id */, false /* is_flush */))
      PERFETTO_DLOG("Failed to dump %" PRIu64, id_and_data_source.first);
  }
}

void HeapprofdProducer::Flush(FlushRequestID flush_id,
                              const DataSourceInstanceID* ids,
                              size_t num_ids) {
  if (num_ids == 0)
    return;

  size_t& flush_in_progress = flushes_in_progress_[flush_id];
  PERFETTO_DCHECK(flush_in_progress == 0);
  flush_in_progress = num_ids;
  for (size_t i = 0; i < num_ids; ++i)
    Dump(ids[i], flush_id, true);
}

void HeapprofdProducer::FinishDataSourceFlush(FlushRequestID flush_id) {
  auto it = flushes_in_progress_.find(flush_id);
  if (it == flushes_in_progress_.end()) {
    PERFETTO_DFATAL("FinishDataSourceFlush id invalid: %" PRIu64, flush_id);
    return;
  }
  size_t& flush_in_progress = it->second;
  if (--flush_in_progress == 0) {
    endpoint_->NotifyFlushComplete(flush_id);
    flushes_in_progress_.erase(flush_id);
  }
}

void HeapprofdProducer::SocketDelegate::OnDisconnect(base::UnixSocket* self) {
  auto it = producer_->pending_processes_.find(self->peer_pid());
  if (it == producer_->pending_processes_.end()) {
    PERFETTO_DFATAL("Unexpected disconnect.");
    return;
  }

  if (self == it->second.sock.get())
    producer_->pending_processes_.erase(it);
}

void HeapprofdProducer::SocketDelegate::OnNewIncomingConnection(
    base::UnixSocket*,
    std::unique_ptr<base::UnixSocket> new_connection) {
  Process peer_process;
  peer_process.pid = new_connection->peer_pid();
  if (!GetCmdlineForPID(peer_process.pid, &peer_process.cmdline))
    PERFETTO_ELOG("Failed to get cmdline for %d", peer_process.pid);

  producer_->HandleClientConnection(std::move(new_connection), peer_process);
}

void HeapprofdProducer::SocketDelegate::OnDataAvailable(
    base::UnixSocket* self) {
  auto it = producer_->pending_processes_.find(self->peer_pid());
  if (it == producer_->pending_processes_.end()) {
    PERFETTO_DFATAL("Unexpected data.");
    return;
  }

  PendingProcess& pending_process = it->second;

  base::ScopedFile fds[kHandshakeSize];
  char buf[1];
  self->Receive(buf, sizeof(buf), fds, base::ArraySize(fds));

  static_assert(kHandshakeSize == 2, "change if below.");
  if (fds[kHandshakeMaps] && fds[kHandshakeMem]) {
    auto ds_it =
        producer_->data_sources_.find(pending_process.data_source_instance_id);
    if (ds_it == producer_->data_sources_.end()) {
      producer_->pending_processes_.erase(it);
      return;
    }

    DataSource& data_source = ds_it->second;
    data_source.process_states.emplace(self->peer_pid(),
                                       &producer_->callsites_);

    PERFETTO_DLOG("%d: Received FDs.", self->peer_pid());
    int raw_fd = pending_process.shmem.fd();
    // TODO(fmayer): Full buffer could deadlock us here.
    self->Send(&data_source.client_configuration,
               sizeof(data_source.client_configuration), &raw_fd, 1,
               base::UnixSocket::BlockingMode::kBlocking);

    UnwindingWorker::HandoffData handoff_data;
    handoff_data.data_source_instance_id =
        pending_process.data_source_instance_id;
    handoff_data.sock = self->ReleaseSocket();
    for (size_t i = 0; i < kHandshakeSize; ++i)
      handoff_data.fds[i] = std::move(fds[i]);
    handoff_data.shmem = std::move(pending_process.shmem);
    handoff_data.client_config = data_source.client_configuration;

    producer_->UnwinderForPID(self->peer_pid())
        .PostHandoffSocket(std::move(handoff_data));
    producer_->pending_processes_.erase(it);
  } else if (fds[kHandshakeMaps] || fds[kHandshakeMem]) {
    PERFETTO_DFATAL("%d: Received partial FDs.", self->peer_pid());
    producer_->pending_processes_.erase(it);
  } else {
    PERFETTO_DLOG("%d: Received no FDs.", self->peer_pid());
  }
}

HeapprofdProducer::DataSource* HeapprofdProducer::GetDataSourceForProcess(
    const Process& proc) {
  for (auto& ds_id_and_datasource : data_sources_) {
    DataSource& ds = ds_id_and_datasource.second;
    if (ConfigTargetsProcess(ds.config, proc, ds.normalized_cmdlines))
      return &ds;
  }
  return nullptr;
}

void HeapprofdProducer::RecordOtherSourcesAsRejected(DataSource* active_ds,
                                                     const Process& proc) {
  for (auto& ds_id_and_datasource : data_sources_) {
    DataSource& ds = ds_id_and_datasource.second;
    if (&ds != active_ds &&
        ConfigTargetsProcess(ds.config, proc, ds.normalized_cmdlines))
      ds.rejected_pids.emplace(proc.pid);
  }
}

void HeapprofdProducer::HandleClientConnection(
    std::unique_ptr<base::UnixSocket> new_connection,
    Process process) {
  DataSource* data_source = GetDataSourceForProcess(process);
  if (!data_source) {
    PERFETTO_LOG("No data source found.");
    return;
  }
  RecordOtherSourcesAsRejected(data_source, process);

  uint64_t shmem_size = data_source->config.shmem_size_bytes();
  if (!shmem_size)
    shmem_size = kDefaultShmemSize;
  if (shmem_size > kMaxShmemSize)
    shmem_size = kMaxShmemSize;

  auto shmem = SharedRingBuffer::Create(shmem_size);
  if (!shmem || !shmem->is_valid()) {
    PERFETTO_LOG("Failed to create shared memory.");
    return;
  }

  pid_t peer_pid = new_connection->peer_pid();
  if (peer_pid != process.pid) {
    PERFETTO_DFATAL("Invalid PID connected.");
    return;
  }

  PendingProcess pending_process;
  pending_process.sock = std::move(new_connection);
  pending_process.data_source_instance_id = data_source->id;
  pending_process.shmem = std::move(*shmem);
  pending_processes_.emplace(peer_pid, std::move(pending_process));
}

void HeapprofdProducer::PostAllocRecord(AllocRecord alloc_rec) {
  // Once we can use C++14, this should be std::moved into the lambda instead.
  AllocRecord* raw_alloc_rec = new AllocRecord(std::move(alloc_rec));
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_this, raw_alloc_rec] {
    if (weak_this)
      weak_this->HandleAllocRecord(std::move(*raw_alloc_rec));
    delete raw_alloc_rec;
  });
}

void HeapprofdProducer::PostFreeRecord(FreeRecord free_rec) {
  // Once we can use C++14, this should be std::moved into the lambda instead.
  FreeRecord* raw_free_rec = new FreeRecord(std::move(free_rec));
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_this, raw_free_rec] {
    if (weak_this)
      weak_this->HandleFreeRecord(std::move(*raw_free_rec));
    delete raw_free_rec;
  });
}

void HeapprofdProducer::PostSocketDisconnected(DataSourceInstanceID ds_id,
                                               pid_t pid,
                                               SharedRingBuffer::Stats stats) {
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_this, ds_id, pid, stats] {
    if (weak_this)
      weak_this->HandleSocketDisconnected(ds_id, pid, stats);
  });
}

void HeapprofdProducer::HandleAllocRecord(AllocRecord alloc_rec) {
  const AllocMetadata& alloc_metadata = alloc_rec.alloc_metadata;
  auto it = data_sources_.find(alloc_rec.data_source_instance_id);
  if (it == data_sources_.end()) {
    PERFETTO_LOG("Invalid data source in alloc record.");
    return;
  }

  DataSource& ds = it->second;
  auto process_state_it = ds.process_states.find(alloc_rec.pid);
  if (process_state_it == ds.process_states.end()) {
    PERFETTO_LOG("Invalid PID in alloc record.");
    return;
  }

  const auto& prefixes = ds.config.skip_symbol_prefix();
  if (!prefixes.empty()) {
    for (FrameData& frame_data : alloc_rec.frames) {
      const std::string& map = frame_data.frame.map_name;
      if (std::find_if(prefixes.cbegin(), prefixes.cend(),
                       [&map](const std::string& prefix) {
                         return base::StartsWith(map, prefix);
                       }) != prefixes.cend()) {
        frame_data.frame.function_name = "FILTERED";
      }
    }
  }

  ProcessState& process_state = process_state_it->second;
  HeapTracker& heap_tracker = process_state.heap_tracker;

  if (alloc_rec.error)
    process_state.unwinding_errors++;
  if (alloc_rec.reparsed_map)
    process_state.map_reparses++;
  process_state.heap_samples++;
  process_state.unwinding_time_us.Add(alloc_rec.unwinding_time_us);
  process_state.total_unwinding_time_us += alloc_rec.unwinding_time_us;

  heap_tracker.RecordMalloc(alloc_rec.frames, alloc_metadata.alloc_address,
                            alloc_metadata.total_size,
                            alloc_metadata.sequence_number,
                            alloc_metadata.clock_monotonic_coarse_timestamp);
}

void HeapprofdProducer::HandleFreeRecord(FreeRecord free_rec) {
  const FreeBatch& free_batch = free_rec.free_batch;
  auto it = data_sources_.find(free_rec.data_source_instance_id);
  if (it == data_sources_.end()) {
    PERFETTO_LOG("Invalid data source in free record.");
    return;
  }

  DataSource& ds = it->second;
  auto process_state_it = ds.process_states.find(free_rec.pid);
  if (process_state_it == ds.process_states.end()) {
    PERFETTO_LOG("Invalid PID in free record.");
    return;
  }

  ProcessState& process_state = process_state_it->second;
  HeapTracker& heap_tracker = process_state.heap_tracker;

  const FreeBatchEntry* entries = free_batch.entries;
  uint64_t num_entries = free_batch.num_entries;
  if (num_entries > kFreeBatchSize) {
    PERFETTO_DFATAL("Malformed free page.");
    return;
  }
  for (size_t i = 0; i < num_entries; ++i) {
    const FreeBatchEntry& entry = entries[i];
    heap_tracker.RecordFree(entry.addr, entry.sequence_number,
                            free_batch.clock_monotonic_coarse_timestamp);
  }
}

void HeapprofdProducer::HandleSocketDisconnected(
    DataSourceInstanceID id,
    pid_t pid,
    SharedRingBuffer::Stats stats) {
  auto it = data_sources_.find(id);
  if (it == data_sources_.end())
    return;
  DataSource& ds = it->second;

  auto process_state_it = ds.process_states.find(pid);
  if (process_state_it == ds.process_states.end())
    return;
  ProcessState& process_state = process_state_it->second;
  process_state.disconnected = true;
  process_state.buffer_overran = stats.num_writes_overflow > 0;
  process_state.buffer_corrupted =
      stats.num_writes_corrupt > 0 || stats.num_reads_corrupt > 0;

  // TODO(fmayer): Dump on process disconnect rather than data source
  // destruction. This prevents us needing to hold onto the bookkeeping data
  // after the process disconnected.
}

}  // namespace profiling
}  // namespace perfetto
