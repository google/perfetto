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

#include "src/profiling/perf/perf_producer.h"

#include <utility>

#include <unistd.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/producer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/ext/tracing/ipc/producer_ipc_client.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "protos/perfetto/config/profiling/perf_event_config.pbzero.h"
#include "src/profiling/common/unwind_support.h"
#include "src/profiling/perf/event_reader.h"

#include <unwindstack/Unwinder.h>

namespace perfetto {
namespace profiling {
namespace {

// TODO(rsavitski): for low sampling rates, look into epoll to detect samples.
constexpr uint32_t kReadTickPeriodMs = 200;
constexpr uint32_t kUnwindTickPeriodMs = 200;
constexpr uint32_t kBookkeepTickPeriodMs = 200;
// TODO(rsavitski): this is better calculated (at setup) from the buffer and
// sample sizes.
constexpr size_t kMaxSamplesPerReadTick = 32;

constexpr size_t kUnwindingMaxFrames = 1000;

constexpr uint32_t kInitialConnectionBackoffMs = 100;
constexpr uint32_t kMaxConnectionBackoffMs = 30 * 1000;

constexpr char kProducerName[] = "perfetto.traced_perf";
constexpr char kDataSourceName[] = "linux.perf";

}  // namespace

PerfProducer::PerfProducer(ProcDescriptorGetter* proc_fd_getter,
                           base::TaskRunner* task_runner)
    : task_runner_(task_runner),
      proc_fd_getter_(proc_fd_getter),
      weak_factory_(this) {
  proc_fd_getter->SetDelegate(this);
}

// TODO(rsavitski): consider configure at setup + enable at start instead.
void PerfProducer::SetupDataSource(DataSourceInstanceID,
                                   const DataSourceConfig&) {}

void PerfProducer::StartDataSource(DataSourceInstanceID instance_id,
                                   const DataSourceConfig& config) {
  PERFETTO_DLOG("StartDataSource(id=%" PRIu64 ", name=%s)", instance_id,
                config.name().c_str());

  if (config.name() != kDataSourceName)
    return;

  base::Optional<EventConfig> event_config = EventConfig::Create(config);
  if (!event_config.has_value()) {
    PERFETTO_ELOG("PerfEventConfig rejected.");
    return;
  }

  base::Optional<EventReader> event_reader =
      EventReader::ConfigureEvents(event_config.value());
  if (!event_reader.has_value()) {
    PERFETTO_ELOG("Failed to set up perf events.");
    return;
  }

  // Construct the data source instance.
  auto it_inserted = data_sources_.emplace(
      std::piecewise_construct, std::forward_as_tuple(instance_id),
      std::forward_as_tuple(std::move(event_reader.value())));

  PERFETTO_CHECK(it_inserted.second);

  // Kick off periodic read task.
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this, instance_id] {
        if (weak_this)
          weak_this->TickDataSourceRead(instance_id);
      },
      kReadTickPeriodMs);

  // Set up unwind queue and kick off a periodic task to process it.
  unwind_queues_.emplace(instance_id, std::deque<UnwindEntry>{});
  task_runner_->PostDelayedTask(
      [weak_this, instance_id] {
        if (weak_this)
          weak_this->TickDataSourceUnwind(instance_id);
      },
      kUnwindTickPeriodMs);

  // Set up bookkeeping queue and kick off a periodic task to process it.
  bookkeping_queues_.emplace(instance_id,
                             std::queue<PerfProducer::BookkeepingEntry>{});
  task_runner_->PostDelayedTask(
      [weak_this, instance_id] {
        if (weak_this)
          weak_this->TickDataSourceBookkeep(instance_id);
      },
      kBookkeepTickPeriodMs);
}

// TODO(rsavitski): stop perf_event before draining ring buffer and internal
// queues (more aggressive flush).
void PerfProducer::StopDataSource(DataSourceInstanceID instance_id) {
  PERFETTO_DLOG("StopDataSource(id=%" PRIu64 ")", instance_id);
  data_sources_.erase(instance_id);
  unwind_queues_.erase(instance_id);
  bookkeping_queues_.erase(instance_id);
}

void PerfProducer::Flush(FlushRequestID flush_id,
                         const DataSourceInstanceID* data_source_ids,
                         size_t num_data_sources) {
  for (size_t i = 0; i < num_data_sources; i++) {
    auto ds_id = data_source_ids[i];
    PERFETTO_DLOG("Flush(id=%" PRIu64 ")", ds_id);

    auto ds_it = data_sources_.find(ds_id);
    if (ds_it != data_sources_.end()) {
      auto unwind_it = unwind_queues_.find(ds_id);
      auto book_it = bookkeping_queues_.find(ds_id);
      PERFETTO_CHECK(unwind_it != unwind_queues_.end());
      PERFETTO_CHECK(book_it != bookkeping_queues_.end());

      ProcessUnwindQueue(&unwind_it->second, &book_it->second, &ds_it->second);
      // TODO(rsavitski): also flush the bookkeeping queue.
      endpoint_->NotifyFlushComplete(flush_id);
    }
  }
}

void PerfProducer::TickDataSourceRead(DataSourceInstanceID ds_id) {
  using Status = DataSource::ProcDescriptors::Status;
  auto it = data_sources_.find(ds_id);
  if (it == data_sources_.end()) {
    PERFETTO_DLOG("Stopping TickDataSourceRead(%zu)",
                  static_cast<size_t>(ds_id));
    return;
  }
  DataSource& ds = it->second;

  // TODO(rsavitski): record the loss in the trace.
  auto lost_events_callback = [ds_id](uint64_t lost_events) {
    PERFETTO_ELOG("DataSource instance [%zu] lost [%" PRIu64 "] events",
                  static_cast<size_t>(ds_id), lost_events);
  };

  for (size_t i = 0; i < kMaxSamplesPerReadTick; i++) {
    base::Optional<ParsedSample> sample =
        ds.event_reader.ReadUntilSample(lost_events_callback);
    if (!sample)
      break;  // caught up to the writer

    pid_t pid = sample->pid;
    if (!sample->regs) {
      // TODO(rsavitski): don't discard if/when doing stackless events.
      PERFETTO_DLOG("Dropping event without register data for pid [%d]",
                    static_cast<int>(pid));
      break;
    }

    // Request proc-fds for the process if this is the first time we see it yet.
    auto& fd_entry = ds.proc_fds[pid];  // created if absent

    if (fd_entry.status == Status::kInitial) {
      PERFETTO_DLOG("New pid: [%d]", static_cast<int>(pid));
      fd_entry.status = Status::kResolving;
      proc_fd_getter_->GetDescriptorsForPid(pid);  // response is async
      PostDescriptorLookupTimeout(ds_id, pid, /*timeout_ms=*/1000);
    }

    if (fd_entry.status == Status::kSkip) {
      PERFETTO_DLOG("Skipping sample for previously poisoned pid [%d]",
                    static_cast<int>(pid));
      continue;
    }

    // Push the sample into a dedicated unwinding queue.
    unwind_queues_[ds_id].emplace_back(std::move(sample.value()));
  }

  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this, ds_id] {
        if (weak_this)
          weak_this->TickDataSourceRead(ds_id);
      },
      kReadTickPeriodMs);
}

// TODO(rsavitski): first-fit makes descriptor request fulfillment not true
// FIFO.
void PerfProducer::OnProcDescriptors(pid_t pid,
                                     base::ScopedFile maps_fd,
                                     base::ScopedFile mem_fd) {
  using Status = DataSource::ProcDescriptors::Status;
  // Find first fit data source that is waiting on descriptors for the process.
  for (auto& it : data_sources_) {
    DataSource& ds = it.second;
    auto proc_fd_it = ds.proc_fds.find(pid);
    if (proc_fd_it != ds.proc_fds.end() &&
        proc_fd_it->second.status == Status::kResolving) {
      proc_fd_it->second.status = Status::kResolved;
      proc_fd_it->second.unwind_state =
          UnwindingMetadata{std::move(maps_fd), std::move(mem_fd)};
      PERFETTO_DLOG("Handed off proc-fds for pid [%d] to DS [%zu]",
                    static_cast<int>(pid), static_cast<size_t>(it.first));
      return;  // done
    }
  }
  PERFETTO_DLOG(
      "Discarding proc-fds for pid [%d] as found no outstanding requests.",
      static_cast<int>(pid));
}

void PerfProducer::PostDescriptorLookupTimeout(DataSourceInstanceID ds_id,
                                               pid_t pid,
                                               uint32_t timeout_ms) {
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this, ds_id, pid] {
        if (weak_this)
          weak_this->HandleDescriptorLookupTimeout(ds_id, pid);
      },
      timeout_ms);
}

void PerfProducer::HandleDescriptorLookupTimeout(DataSourceInstanceID ds_id,
                                                 pid_t pid) {
  using Status = DataSource::ProcDescriptors::Status;
  auto ds_it = data_sources_.find(ds_id);
  if (ds_it == data_sources_.end())
    return;

  // If the request is still outstanding, poison the pid for this source.
  DataSource& ds = ds_it->second;
  auto proc_fd_it = ds.proc_fds.find(pid);
  if (proc_fd_it != ds.proc_fds.end() &&
      proc_fd_it->second.status == Status::kResolving) {
    proc_fd_it->second.status = Status::kSkip;
    PERFETTO_DLOG("Descriptor lookup timeout of pid [%d] for DS [%zu]",
                  static_cast<int>(pid), static_cast<size_t>(ds_it->first));
  }
}

void PerfProducer::TickDataSourceUnwind(DataSourceInstanceID ds_id) {
  auto ds_it = data_sources_.find(ds_id);
  if (ds_it == data_sources_.end()) {
    PERFETTO_DLOG("Stopping TickDataSourceUnwind(%zu)",
                  static_cast<size_t>(ds_id));
    return;
  }

  auto unwind_it = unwind_queues_.find(ds_id);
  auto book_it = bookkeping_queues_.find(ds_id);
  PERFETTO_CHECK(unwind_it != unwind_queues_.end());
  PERFETTO_CHECK(book_it != bookkeping_queues_.end());

  ProcessUnwindQueue(&unwind_it->second, &book_it->second, &ds_it->second);

  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this, ds_id] {
        if (weak_this)
          weak_this->TickDataSourceUnwind(ds_id);
      },
      kUnwindTickPeriodMs);
}

// TODO(rsavitski): reader can purge kResolving entries from the start once the
// queue grows too large.
// TODO(rsavitski): DataSource input won't be needed once fd-tracking in the
// unwinder is separated from fd-tracking in the reading frontend.
void PerfProducer::ProcessUnwindQueue(
    std::deque<UnwindEntry>* input_queue,
    std::queue<BookkeepingEntry>* output_queue,
    DataSource* ds_ptr) {
  using Status = DataSource::ProcDescriptors::Status;
  auto& queue = *input_queue;
  auto& ds = *ds_ptr;

  // Iterate over the queue, handling unwindable samples, and then marking them
  // as processed.
  size_t num_samples = queue.size();
  for (size_t i = 0; i < num_samples; i++) {
    UnwindEntry& entry = queue[i];
    if (!entry.valid)
      continue;  // already processed

    ParsedSample& sample = entry.sample;
    auto proc_fd_it = ds.proc_fds.find(sample.pid);
    PERFETTO_CHECK(proc_fd_it != ds.proc_fds.end());  // must be present

    auto fd_status = proc_fd_it->second.status;
    PERFETTO_CHECK(fd_status != Status::kInitial);

    // Giving up on the sample (proc-fd lookup timed out).
    if (fd_status == Status::kSkip) {
      PERFETTO_DLOG("Skipping sample for pid [%d]",
                    static_cast<int>(sample.pid));
      entry.valid = false;
      continue;
    }

    // Still waiting on the proc-fds.
    if (fd_status == Status::kResolving) {
      PERFETTO_DLOG("Still resolving sample for pid [%d]",
                    static_cast<int>(sample.pid));
      continue;
    }

    // Sample ready - process it.
    if (fd_status == Status::kResolved) {
      PerfProducer::BookkeepingEntry unwound_sample =
          UnwindSample(std::move(sample), &proc_fd_it->second);

      output_queue->push(std::move(unwound_sample));

      entry.valid = false;
      continue;
    }
  }

  // Pop all leading processed entries.
  for (size_t i = 0; i < num_samples; i++) {
    PERFETTO_DCHECK(queue.size() > 0);
    if (queue.front().valid)
      break;
    queue.pop_front();
  }

  PERFETTO_DLOG("Unwind queue drain: [%zu]->[%zu]", num_samples, queue.size());
}

PerfProducer::BookkeepingEntry PerfProducer::UnwindSample(
    ParsedSample sample,
    DataSource::ProcDescriptors* process_state) {
  PerfProducer::BookkeepingEntry ret;
  ret.pid = sample.pid;
  ret.tid = sample.tid;
  ret.timestamp = sample.timestamp;

  auto& unwind_state = process_state->unwind_state;

  // Overlay the stack bytes over /proc/<pid>/mem.
  std::shared_ptr<unwindstack::Memory> overlay_memory =
      std::make_shared<StackOverlayMemory>(
          unwind_state.fd_mem, sample.regs->sp(),
          reinterpret_cast<uint8_t*>(sample.stack.data()), sample.stack.size());

  // Unwindstack clobbers registers, so make a copy in case we need to retry.
  auto working_regs = std::unique_ptr<unwindstack::Regs>{sample.regs->Clone()};

  uint8_t error_code = unwindstack::ERROR_NONE;
  unwindstack::Unwinder unwinder(kUnwindingMaxFrames, &unwind_state.fd_maps,
                                 working_regs.get(), overlay_memory);

  for (int attempt = 0; attempt < 2; attempt++) {
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
    unwinder.SetJitDebug(unwind_state.jit_debug.get(), working_regs->Arch());
    unwinder.SetDexFiles(unwind_state.dex_files.get(), working_regs->Arch());
#endif
    unwinder.Unwind(/*initial_map_names_to_skip=*/nullptr,
                    /*map_suffixes_to_ignore=*/nullptr);
    error_code = unwinder.LastErrorCode();
    if (error_code != unwindstack::ERROR_INVALID_MAP)
      break;

    // Otherwise, reparse the maps, and possibly retry the unwind.
    PERFETTO_DLOG("Reparsing maps");
    unwind_state.ReparseMaps();
  }

  PERFETTO_DLOG("Frames:");
  std::vector<unwindstack::FrameData> frames = unwinder.ConsumeFrames();
  for (unwindstack::FrameData& frame : frames) {
    PERFETTO_DLOG("%s", unwinder.FormatFrame(frame).c_str());
    ret.frames.emplace_back(unwind_state.AnnotateFrame(std::move(frame)));
  }

  if (error_code != unwindstack::ERROR_NONE)
    ret.unwind_error = true;

  return ret;
}

void PerfProducer::TickDataSourceBookkeep(DataSourceInstanceID ds_id) {
  auto q_it = bookkeping_queues_.find(ds_id);
  if (q_it == bookkeping_queues_.end()) {
    return;
  }

  auto& queue = q_it->second;
  while (!queue.empty()) {
    BookkeepingEntry& entry = queue.front();
    PERFETTO_DLOG("Bookkeeping sample: pid:[%d], ts:[%" PRIu64 "]",
                  static_cast<int>(entry.pid), entry.timestamp);

    queue.pop();
  }

  // Repost tick.
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this, ds_id] {
        if (weak_this)
          weak_this->TickDataSourceBookkeep(ds_id);
      },
      kBookkeepTickPeriodMs);
}

void PerfProducer::ConnectWithRetries(const char* socket_name) {
  PERFETTO_DCHECK(state_ == kNotStarted);
  state_ = kNotConnected;

  ResetConnectionBackoff();
  producer_socket_name_ = socket_name;
  ConnectService();
}

void PerfProducer::ConnectService() {
  PERFETTO_DCHECK(state_ == kNotConnected);
  state_ = kConnecting;
  endpoint_ = ProducerIPCClient::Connect(producer_socket_name_, this,
                                         kProducerName, task_runner_);
}

void PerfProducer::IncreaseConnectionBackoff() {
  connection_backoff_ms_ *= 2;
  if (connection_backoff_ms_ > kMaxConnectionBackoffMs)
    connection_backoff_ms_ = kMaxConnectionBackoffMs;
}

void PerfProducer::ResetConnectionBackoff() {
  connection_backoff_ms_ = kInitialConnectionBackoffMs;
}

void PerfProducer::OnConnect() {
  PERFETTO_DCHECK(state_ == kConnecting);
  state_ = kConnected;
  ResetConnectionBackoff();
  PERFETTO_LOG("Connected to the service");

  DataSourceDescriptor desc;
  desc.set_name(kDataSourceName);
  endpoint_->RegisterDataSource(desc);
}

void PerfProducer::OnDisconnect() {
  PERFETTO_DCHECK(state_ == kConnected || state_ == kConnecting);
  PERFETTO_LOG("Disconnected from tracing service");

  auto weak_producer = weak_factory_.GetWeakPtr();
  if (state_ == kConnected)
    return task_runner_->PostTask([weak_producer] {
      if (weak_producer)
        weak_producer->Restart();
    });

  state_ = kNotConnected;
  IncreaseConnectionBackoff();
  task_runner_->PostDelayedTask(
      [weak_producer] {
        if (weak_producer)
          weak_producer->ConnectService();
      },
      connection_backoff_ms_);
}

void PerfProducer::Restart() {
  // We lost the connection with the tracing service. At this point we need
  // to reset all the data sources. Trying to handle that manually is going to
  // be error prone. What we do here is simply destroy the instance and
  // recreate it again.
  base::TaskRunner* task_runner = task_runner_;
  const char* socket_name = producer_socket_name_;
  ProcDescriptorGetter* proc_fd_getter = proc_fd_getter_;

  // Invoke destructor and then the constructor again.
  this->~PerfProducer();
  new (this) PerfProducer(proc_fd_getter, task_runner);

  ConnectWithRetries(socket_name);
}

}  // namespace profiling
}  // namespace perfetto
