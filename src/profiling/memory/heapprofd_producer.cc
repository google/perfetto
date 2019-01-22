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
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "perfetto/tracing/ipc/producer_ipc_client.h"

namespace perfetto {
namespace profiling {
namespace {
constexpr char kHeapprofdDataSource[] = "android.heapprofd";
constexpr size_t kUnwinderQueueSize = 1000;
constexpr size_t kBookkeepingQueueSize = 1000;
constexpr size_t kUnwinderThreads = 5;
constexpr int kHeapprofdSignal = 36;

constexpr uint32_t kInitialConnectionBackoffMs = 100;
constexpr uint32_t kMaxConnectionBackoffMs = 30 * 1000;

ClientConfiguration MakeClientConfiguration(const DataSourceConfig& cfg) {
  ClientConfiguration client_config;
  client_config.interval = cfg.heapprofd_config().sampling_interval_bytes();
  return client_config;
}

}  // namespace

// We create kUnwinderThreads unwinding threads and one bookeeping thread.
// The bookkeeping thread is singleton in order to avoid expensive and
// complicated synchronisation in the bookkeeping.
//
// We wire up the system by creating BoundedQueues between the threads. The main
// thread runs the TaskRunner driving the SocketListener. The unwinding thread
// takes the data received by the SocketListener and if it is a malloc does
// stack unwinding, and if it is a free just forwards the content of the record
// to the bookkeeping thread.
//
//             +--------------+
//             |SocketListener|
//             +------+-------+
//                    |
//          +--UnwindingRecord -+
//          |                   |
// +--------v-------+   +-------v--------+
// |Unwinding Thread|   |Unwinding Thread|
// +--------+-------+   +-------+--------+
//          |                   |
//          +-BookkeepingRecord +
//                    |
//           +--------v---------+
//           |Bookkeeping Thread|
//           +------------------+
HeapprofdProducer::HeapprofdProducer(HeapprofdMode mode,
                                     base::TaskRunner* task_runner)
    : mode_(mode),
      task_runner_(task_runner),
      bookkeeping_queue_(kBookkeepingQueueSize),
      bookkeeping_th_([this] { bookkeeping_thread_.Run(&bookkeeping_queue_); }),
      unwinder_queues_(MakeUnwinderQueues(kUnwinderThreads)),
      unwinding_threads_(MakeUnwindingThreads(kUnwinderThreads)),
      socket_listener_(MakeSocketListenerCallback(), &bookkeeping_thread_),
      target_pid_(base::kInvalidPid),
      weak_factory_(this) {
  if (mode == HeapprofdMode::kCentral) {
    listening_socket_ = MakeListeningSocket();
  }
}

HeapprofdProducer::~HeapprofdProducer() {
  bookkeeping_queue_.Shutdown();
  for (auto& queue : unwinder_queues_) {
    queue.Shutdown();
  }
  bookkeeping_th_.join();
  for (std::thread& th : unwinding_threads_) {
    th.join();
  }
}

void HeapprofdProducer::SetTargetProcess(pid_t target_pid,
                                         std::string target_cmdline) {
  target_pid_ = target_pid;
  target_cmdline_ = target_cmdline;
}

bool HeapprofdProducer::SourceMatchesTarget(const HeapprofdConfig& cfg) {
  if (cfg.all())
    return true;
  if (target_pid_ != base::kInvalidPid &&
      std::find(cfg.pid().cbegin(), cfg.pid().cend(),
                static_cast<uint64_t>(target_pid_)) != cfg.pid().cend()) {
    return true;
  }
  if (!target_cmdline_.empty() &&
      std::find(cfg.process_cmdline().cbegin(), cfg.process_cmdline().cend(),
                target_cmdline_) != cfg.process_cmdline().cend()) {
    return true;
  }

  return false;
}

void HeapprofdProducer::AdoptConnectedSockets(
    std::vector<base::ScopedFile> inherited_sockets) {
  PERFETTO_DCHECK(mode_ == HeapprofdMode::kChild);

  auto weak_producer = weak_factory_.GetWeakPtr();
  for (auto& scoped_fd : inherited_sockets) {
    // Manually enqueue the on-connection callback. Pass the raw fd into the
    // closure as we cannot easily move-capture in c++11.
    int fd = scoped_fd.release();
    task_runner_->PostTask([weak_producer, fd] {
      if (!weak_producer)
        return;

      auto socket = base::UnixSocket::AdoptConnected(
          base::ScopedFile(fd), &weak_producer->socket_listener_,
          weak_producer->task_runner_, base::SockType::kStream);

      // The forked heapprofd will not normally be able to read the target's
      // cmdline under procfs, so pass peer's description explicitly.
      Process process{weak_producer->target_pid_,
                      weak_producer->target_cmdline_};

      weak_producer->socket_listener_.HandleClientConnection(
          std::move(socket), std::move(process));
    });
  }
}

// TODO(fmayer): Delete once we have generic reconnect logic.
void HeapprofdProducer::OnConnect() {
  PERFETTO_DCHECK(state_ == kConnecting);
  state_ = kConnected;
  ResetConnectionBackoff();
  PERFETTO_LOG("Connected to the service");

  DataSourceDescriptor desc;
  desc.set_name(kHeapprofdDataSource);
  endpoint_->RegisterDataSource(desc);
}

// TODO(fmayer): Delete once we have generic reconnect logic.
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
        weak_producer->Connect();
      },
      connection_backoff_ms_);
}

void HeapprofdProducer::SetupDataSource(DataSourceInstanceID id,
                                        const DataSourceConfig& cfg) {
  PERFETTO_DLOG("Setting up data source.");
  const HeapprofdConfig& heapprofd_config = cfg.heapprofd_config();
  if (heapprofd_config.all() && !heapprofd_config.pid().empty())
    PERFETTO_ELOG("No point setting all and pid");
  if (heapprofd_config.all() && !heapprofd_config.process_cmdline().empty())
    PERFETTO_ELOG("No point setting all and process_cmdline");

  if (cfg.name() != kHeapprofdDataSource) {
    PERFETTO_DLOG("Invalid data source name.");
    return;
  }

  // Child mode is only interested in data sources matching the
  // already-connected process.
  if (mode_ == HeapprofdMode::kChild &&
      !SourceMatchesTarget(heapprofd_config)) {
    PERFETTO_DLOG("Child mode skipping setup of unrelated data source.");
    return;
  }

  auto it = data_sources_.find(id);
  if (it != data_sources_.end()) {
    PERFETTO_DFATAL("Received duplicated data source instance id: %" PRIu64,
                    id);
    return;
  }

  DataSource data_source;

  ProcessSetSpec process_set_spec{};
  process_set_spec.all = heapprofd_config.all();
  process_set_spec.client_configuration = MakeClientConfiguration(cfg);
  process_set_spec.pids.insert(heapprofd_config.pid().cbegin(),
                               heapprofd_config.pid().cend());
  process_set_spec.process_cmdline.insert(
      heapprofd_config.process_cmdline().cbegin(),
      heapprofd_config.process_cmdline().cend());

  data_source.processes =
      socket_listener_.process_matcher().AwaitProcessSetSpec(
          std::move(process_set_spec));

  auto buffer_id = static_cast<BufferID>(cfg.target_buffer());
  data_source.trace_writer = endpoint_->CreateTraceWriter(buffer_id);

  data_sources_.emplace(id, std::move(data_source));
  PERFETTO_DLOG("Set up data source.");
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

void HeapprofdProducer::StartDataSource(DataSourceInstanceID id,
                                        const DataSourceConfig& cfg) {
  PERFETTO_DLOG("Start DataSource");
  const HeapprofdConfig& heapprofd_config = cfg.heapprofd_config();

  // Child mode is only interested in data sources matching the
  // already-connected process.
  if (mode_ == HeapprofdMode::kChild &&
      !SourceMatchesTarget(heapprofd_config)) {
    PERFETTO_DLOG("Child mode skipping start of unrelated data source.");
    return;
  }

  auto it = data_sources_.find(id);
  if (it == data_sources_.end()) {
    PERFETTO_DFATAL("Received invalid data source instance to start: %" PRIu64,
                    id);
    return;
  }
  DataSource& data_source = it->second;

  // Central daemon - set system properties for any targets that start later,
  // and signal already-running targets to start the profiling client.
  if (mode_ == HeapprofdMode::kCentral) {
    if (heapprofd_config.all())
      data_source.properties.emplace_back(properties_.SetAll());

    for (std::string cmdline : heapprofd_config.process_cmdline())
      data_source.properties.emplace_back(
          properties_.SetProperty(std::move(cmdline)));

    std::set<pid_t> pids;
    if (heapprofd_config.all())
      FindAllProfilablePids(&pids);
    for (uint64_t pid : heapprofd_config.pid())
      pids.emplace(static_cast<pid_t>(pid));

    if (!heapprofd_config.process_cmdline().empty())
      FindPidsForCmdlines(heapprofd_config.process_cmdline(), &pids);

    for (pid_t pid : pids) {
      PERFETTO_DLOG("Sending %d to %d", kHeapprofdSignal, pid);
      if (kill(pid, kHeapprofdSignal) != 0) {
        PERFETTO_DPLOG("kill");
      }
    }
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

// TODO(rsavitski): for now, shut down child heapprofd as soon as the first
// matching data source is stopped (even if there are other active matching data
// sources). Instead, we could be called back by SocketListener::Disconnect to
// handle not only the last data source reference being stopped, but also the
// client disconnecting prematurely. Although, still need to look at whether
// child mode heapprofd needs to distinguish between causes of the client
// reference being torn down.
void HeapprofdProducer::StopDataSource(DataSourceInstanceID id) {
  // DataSource holds ProfilingSession handles which on being destructed tear
  // down the profiling on the client.

  if (mode_ == HeapprofdMode::kChild) {
    if (data_sources_.erase(id) == 1) {
      PERFETTO_DLOG("Child mode exiting due to stopped data source.");
      TerminateProcess(/*exit_status=*/0);  // does not return
    }

  } else {  // kCentral
    if (data_sources_.erase(id) != 1)
      PERFETTO_DFATAL("Trying to stop non existing data source: %" PRIu64, id);
  }
}

void HeapprofdProducer::OnTracingSetup() {}

bool HeapprofdProducer::Dump(DataSourceInstanceID id,
                             FlushRequestID flush_id,
                             bool has_flush_id) {
  PERFETTO_DLOG("Dumping %" PRIu64 ", flush: %d", id, has_flush_id);
  auto it = data_sources_.find(id);
  if (it == data_sources_.end()) {
    return false;
  }

  const DataSource& data_source = it->second;
  BookkeepingRecord record{};
  record.record_type = BookkeepingRecord::Type::Dump;
  DumpRecord& dump_record = record.dump_record;
  std::set<pid_t> pids = data_source.processes.GetPIDs();
  dump_record.pids.insert(dump_record.pids.begin(), pids.cbegin(), pids.cend());
  dump_record.trace_writer = data_source.trace_writer;

  auto weak_producer = weak_factory_.GetWeakPtr();
  base::TaskRunner* task_runner = task_runner_;
  if (has_flush_id) {
    dump_record.callback = [task_runner, weak_producer, flush_id] {
      task_runner->PostTask([weak_producer, flush_id] {
        if (weak_producer)
          return weak_producer->FinishDataSourceFlush(flush_id);
      });
    };
  } else {
    dump_record.callback = [] {};
  }

  bookkeeping_queue_.Add(std::move(record));
  return true;
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

std::function<void(UnwindingRecord)>
HeapprofdProducer::MakeSocketListenerCallback() {
  return [this](UnwindingRecord record) {
    unwinder_queues_[static_cast<size_t>(record.pid) % kUnwinderThreads].Add(
        std::move(record));
  };
}

std::vector<BoundedQueue<UnwindingRecord>>
HeapprofdProducer::MakeUnwinderQueues(size_t n) {
  std::vector<BoundedQueue<UnwindingRecord>> ret(n);
  for (size_t i = 0; i < n; ++i)
    ret[i].SetCapacity(kUnwinderQueueSize);
  return ret;
}

std::vector<std::thread> HeapprofdProducer::MakeUnwindingThreads(size_t n) {
  std::vector<std::thread> ret;
  for (size_t i = 0; i < n; ++i) {
    ret.emplace_back([this, i] {
      UnwindingMainLoop(&unwinder_queues_[i], &bookkeeping_queue_);
    });
  }
  return ret;
}

std::unique_ptr<base::UnixSocket> HeapprofdProducer::MakeListeningSocket() {
  const char* sock_fd = getenv(kHeapprofdSocketEnvVar);
  if (sock_fd == nullptr) {
    unlink(kHeapprofdSocketFile);
    return base::UnixSocket::Listen(kHeapprofdSocketFile, &socket_listener_,
                                    task_runner_);
  }
  char* end;
  int raw_fd = static_cast<int>(strtol(sock_fd, &end, 10));
  if (*end != '\0')
    PERFETTO_FATAL("Invalid %s. Expected decimal integer.",
                   kHeapprofdSocketEnvVar);
  return base::UnixSocket::Listen(base::ScopedFile(raw_fd), &socket_listener_,
                                  task_runner_);
}

// TODO(fmayer): Delete these and use ReconnectingProducer once submitted
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

void HeapprofdProducer::ConnectWithRetries(const char* socket_name) {
  PERFETTO_DCHECK(state_ == kNotStarted);
  state_ = kNotConnected;

  ResetConnectionBackoff();
  producer_sock_name_ = socket_name;
  Connect();
}

void HeapprofdProducer::DumpAll() {
  for (const auto& id_and_data_source : data_sources_) {
    if (!Dump(id_and_data_source.first, 0 /* flush_id */, false /* is_flush */))
      PERFETTO_DLOG("Failed to dump %" PRIu64, id_and_data_source.first);
  }
}

void HeapprofdProducer::Connect() {
  PERFETTO_DCHECK(state_ == kNotConnected);
  state_ = kConnecting;
  endpoint_ = ProducerIPCClient::Connect(producer_sock_name_, this,
                                         "android.heapprofd", task_runner_);
}

void HeapprofdProducer::IncreaseConnectionBackoff() {
  connection_backoff_ms_ *= 2;
  if (connection_backoff_ms_ > kMaxConnectionBackoffMs)
    connection_backoff_ms_ = kMaxConnectionBackoffMs;
}

void HeapprofdProducer::ResetConnectionBackoff() {
  connection_backoff_ms_ = kInitialConnectionBackoffMs;
}

// TODO(rsavitski): would be cleaner to shut down the event loop instead
// (letting main exit). One test-friendly approach is to supply a shutdown
// callback in the constructor.
__attribute__((noreturn)) void HeapprofdProducer::TerminateProcess(
    int exit_status) {
  PERFETTO_CHECK(mode_ == HeapprofdMode::kChild);
  exit(exit_status);
}

}  // namespace profiling
}  // namespace perfetto
