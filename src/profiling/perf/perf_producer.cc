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

#include <unwindstack/Error.h>
#include <unwindstack/Unwinder.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/metatrace.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/producer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/ext/tracing/ipc/producer_ipc_client.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "src/profiling/common/callstack_trie.h"
#include "src/profiling/common/unwind_support.h"
#include "src/profiling/perf/event_reader.h"

#include "protos/perfetto/config/profiling/perf_event_config.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace profiling {
namespace {

// TODO(rsavitski): for low sampling rates, look into epoll to detect samples.
constexpr uint32_t kReadTickPeriodMs = 200;
constexpr uint32_t kUnwindTickPeriodMs = 200;
// TODO(rsavitski): this is better calculated (at setup) from the buffer and
// sample sizes.
constexpr size_t kMaxSamplesPerCpuPerReadTick = 32;
// TODO(rsavitski): consider making this part of the config (for slow testing
// platforms).
constexpr uint32_t kProcDescriptorTimeoutMs = 400;

constexpr size_t kUnwindingMaxFrames = 1000;

constexpr uint32_t kInitialConnectionBackoffMs = 100;
constexpr uint32_t kMaxConnectionBackoffMs = 30 * 1000;

constexpr char kProducerName[] = "perfetto.traced_perf";
constexpr char kDataSourceName[] = "linux.perf";

size_t NumberOfCpus() {
  return static_cast<size_t>(sysconf(_SC_NPROCESSORS_CONF));
}

uint64_t NowMs() {
  return static_cast<uint64_t>(base::GetWallTimeMs().count());
}

protos::pbzero::Profiling::CpuMode ToCpuModeEnum(uint16_t perf_cpu_mode) {
  using Profiling = protos::pbzero::Profiling;
  switch (perf_cpu_mode) {
    case PERF_RECORD_MISC_KERNEL:
      return Profiling::MODE_KERNEL;
    case PERF_RECORD_MISC_USER:
      return Profiling::MODE_USER;
    case PERF_RECORD_MISC_HYPERVISOR:
      return Profiling::MODE_HYPERVISOR;
    case PERF_RECORD_MISC_GUEST_KERNEL:
      return Profiling::MODE_GUEST_KERNEL;
    case PERF_RECORD_MISC_GUEST_USER:
      return Profiling::MODE_GUEST_USER;
    default:
      return Profiling::MODE_UNKNOWN;
  }
}

protos::pbzero::Profiling::StackUnwindError ToProtoEnum(
    unwindstack::ErrorCode error_code) {
  using Profiling = protos::pbzero::Profiling;
  switch (error_code) {
    case unwindstack::ERROR_NONE:
      return Profiling::UNWIND_ERROR_NONE;
    case unwindstack::ERROR_MEMORY_INVALID:
      return Profiling::UNWIND_ERROR_MEMORY_INVALID;
    case unwindstack::ERROR_UNWIND_INFO:
      return Profiling::UNWIND_ERROR_UNWIND_INFO;
    case unwindstack::ERROR_UNSUPPORTED:
      return Profiling::UNWIND_ERROR_UNSUPPORTED;
    case unwindstack::ERROR_INVALID_MAP:
      return Profiling::UNWIND_ERROR_INVALID_MAP;
    case unwindstack::ERROR_MAX_FRAMES_EXCEEDED:
      return Profiling::UNWIND_ERROR_MAX_FRAMES_EXCEEDED;
    case unwindstack::ERROR_REPEATED_FRAME:
      return Profiling::UNWIND_ERROR_REPEATED_FRAME;
    case unwindstack::ERROR_INVALID_ELF:
      return Profiling::UNWIND_ERROR_INVALID_ELF;
  }
  return Profiling::UNWIND_ERROR_UNKNOWN;
}

}  // namespace

PerfProducer::PerfProducer(ProcDescriptorGetter* proc_fd_getter,
                           base::TaskRunner* task_runner)
    : task_runner_(task_runner),
      proc_fd_getter_(proc_fd_getter),
      weak_factory_(this) {
  proc_fd_getter->SetDelegate(this);

  // Enable the static unwinding cache, clearing it first in case we're
  // reconstructing the class in |Restart|.
  // TODO(rsavitski): the toggling needs to be done on the same thread as
  // unwinding (right now this is on the same primary thread).
  unwindstack::Elf::SetCachingEnabled(false);
  unwindstack::Elf::SetCachingEnabled(true);
}

// TODO(rsavitski): consider configure at setup + enable at start instead.
void PerfProducer::SetupDataSource(DataSourceInstanceID,
                                   const DataSourceConfig&) {}

void PerfProducer::StartDataSource(DataSourceInstanceID instance_id,
                                   const DataSourceConfig& config) {
  PERFETTO_LOG("StartDataSource(%zu, %s)", static_cast<size_t>(instance_id),
               config.name().c_str());

  if (config.name() == MetatraceWriter::kDataSourceName) {
    StartMetatraceSource(instance_id,
                         static_cast<BufferID>(config.target_buffer()));
    return;
  }

  // linux.perf data source
  if (config.name() != kDataSourceName)
    return;

  base::Optional<EventConfig> event_config = EventConfig::Create(config);
  if (!event_config.has_value()) {
    PERFETTO_ELOG("PerfEventConfig rejected.");
    return;
  }

  // TODO(rsavitski): consider supporting specific cpu subsets.
  if (!event_config->target_all_cpus()) {
    PERFETTO_ELOG("PerfEventConfig{all_cpus} required");
    return;
  }
  size_t num_cpus = NumberOfCpus();
  std::vector<EventReader> per_cpu_readers;
  for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
    base::Optional<EventReader> event_reader =
        EventReader::ConfigureEvents(cpu, event_config.value());
    if (!event_reader.has_value()) {
      PERFETTO_ELOG("Failed to set up perf events for cpu%" PRIu32
                    ", discarding data source.",
                    cpu);
      return;
    }
    per_cpu_readers.emplace_back(std::move(event_reader.value()));
  }

  auto buffer_id = static_cast<BufferID>(config.target_buffer());
  auto writer = endpoint_->CreateTraceWriter(buffer_id);

  // Construct the data source instance.
  std::map<DataSourceInstanceID, DataSource>::iterator ds_it;
  bool inserted;
  std::tie(ds_it, inserted) = data_sources_.emplace(
      std::piecewise_construct, std::forward_as_tuple(instance_id),
      std::forward_as_tuple(std::move(writer), std::move(per_cpu_readers)));
  PERFETTO_CHECK(inserted);

  // Write out a packet to initialize the incremental state for this sequence.
  InterningOutputTracker::WriteFixedInterningsPacket(
      ds_it->second.trace_writer.get());

  // Kick off periodic read task.
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this, instance_id] {
        if (weak_this)
          weak_this->TickDataSourceRead(instance_id);
      },
      kReadTickPeriodMs - (NowMs() % kReadTickPeriodMs));

  // Set up unwind queue and kick off a periodic task to process it.
  unwind_queues_.emplace(instance_id, std::deque<UnwindEntry>{});
  task_runner_->PostDelayedTask(
      [weak_this, instance_id] {
        if (weak_this)
          weak_this->TickDataSourceUnwind(instance_id);
      },
      kUnwindTickPeriodMs - (NowMs() % kUnwindTickPeriodMs));
}

void PerfProducer::StopDataSource(DataSourceInstanceID instance_id) {
  PERFETTO_LOG("StopDataSource(%zu)", static_cast<size_t>(instance_id));

  // Metatrace: stop immediately (will miss the events from the
  // asynchronous shutdown of the primary data source).
  auto meta_it = metatrace_writers_.find(instance_id);
  if (meta_it != metatrace_writers_.end()) {
    meta_it->second.WriteAllAndFlushTraceWriter([] {});
    metatrace_writers_.erase(meta_it);
    return;
  }

  auto ds_it = data_sources_.find(instance_id);
  if (ds_it == data_sources_.end())
    return;

  // Start shutting down the reading frontend, which will propagate the stop
  // further as the intermediate buffers are cleared.
  DataSource& ds = ds_it->second;
  InitiateReaderStop(&ds);
}

// The perf data sources ignore flush requests, as flushing would be
// unnecessarily complicated given out-of-order unwinding and proc-fd timeouts.
// Instead of responding to explicit flushes, we can ensure that we're otherwise
// well-behaved (do not reorder packets too much), and let the service scrape
// the SMB.
void PerfProducer::Flush(FlushRequestID flush_id,
                         const DataSourceInstanceID* data_source_ids,
                         size_t num_data_sources) {
  bool should_ack_flush = false;
  for (size_t i = 0; i < num_data_sources; i++) {
    auto ds_id = data_source_ids[i];
    PERFETTO_DLOG("Flush(%zu)", static_cast<size_t>(ds_id));

    auto meta_it = metatrace_writers_.find(ds_id);
    if (meta_it != metatrace_writers_.end()) {
      meta_it->second.WriteAllAndFlushTraceWriter([] {});
      should_ack_flush = true;
    }
    if (data_sources_.find(ds_id) != data_sources_.end()) {
      should_ack_flush = true;
    }
  }
  if (should_ack_flush)
    endpoint_->NotifyFlushComplete(flush_id);
}

void PerfProducer::TickDataSourceRead(DataSourceInstanceID ds_id) {
  auto it = data_sources_.find(ds_id);
  if (it == data_sources_.end()) {
    PERFETTO_DLOG("TickDataSourceRead(%zu): source gone",
                  static_cast<size_t>(ds_id));
    return;
  }
  DataSource& ds = it->second;

  PERFETTO_METATRACE_SCOPED(TAG_PRODUCER, PROFILER_READ_TICK);

  // Make a pass over all per-cpu readers.
  bool more_records_available = false;
  for (EventReader& reader : ds.per_cpu_readers) {
    if (ReadAndParsePerCpuBuffer(&reader, kMaxSamplesPerCpuPerReadTick, ds_id,
                                 &ds)) {
      more_records_available = true;
    }
  }

  if (PERFETTO_UNLIKELY(ds.reader_stopping) && !more_records_available) {
    InitiateUnwindStop(&ds);
  } else {
    // otherwise, keep reading
    auto weak_this = weak_factory_.GetWeakPtr();
    task_runner_->PostDelayedTask(
        [weak_this, ds_id] {
          if (weak_this)
            weak_this->TickDataSourceRead(ds_id);
        },
        kReadTickPeriodMs - (NowMs() % kReadTickPeriodMs));
  }
}

bool PerfProducer::ReadAndParsePerCpuBuffer(EventReader* reader,
                                            size_t max_samples,
                                            DataSourceInstanceID ds_id,
                                            DataSource* ds) {
  using Status = DataSource::ProcDescriptors::Status;
  PERFETTO_METATRACE_SCOPED(TAG_PRODUCER, PROFILER_READ_CPU);

  // If the kernel ring buffer dropped data, record it in the trace.
  size_t cpu = reader->cpu();
  auto records_lost_callback = [this, ds_id, cpu](uint64_t records_lost) {
    auto weak_this = weak_factory_.GetWeakPtr();
    task_runner_->PostTask([weak_this, ds_id, cpu, records_lost] {
      if (weak_this)
        weak_this->EmitRingBufferLoss(ds_id, cpu, records_lost);
    });
  };

  for (size_t i = 0; i < max_samples; i++) {
    base::Optional<ParsedSample> sample =
        reader->ReadUntilSample(records_lost_callback);
    if (!sample) {
      return false;  // caught up to the writer
    }

    if (!sample->regs) {
      // skip kernel threads/workers
      continue;
    }

    // Request proc-fds for the process if this is the first time we see it.
    pid_t pid = sample->pid;
    auto& fd_entry = ds->proc_fds[pid];  // created if absent

    if (fd_entry.status == Status::kInitial) {
      PERFETTO_DLOG("New pid: [%d]", static_cast<int>(pid));
      fd_entry.status = Status::kResolving;
      proc_fd_getter_->GetDescriptorsForPid(pid);  // response is async
      PostDescriptorLookupTimeout(ds_id, pid, kProcDescriptorTimeoutMs);
    }

    if (fd_entry.status == Status::kSkip) {
      PERFETTO_DLOG("Skipping sample for previously poisoned pid [%d]",
                    static_cast<int>(pid));
      PostEmitSkippedSample(ds_id, ProfilerStage::kRead,
                            std::move(sample.value()));
      continue;
    }

    // Push the sample into a dedicated unwinding queue.
    unwind_queues_[ds_id].emplace_back(std::move(sample.value()));

    // Metatrace: counter sensible only when there's a single active source.
    PERFETTO_METATRACE_COUNTER(TAG_PRODUCER, PROFILER_UNWIND_QUEUE_SZ,
                               unwind_queues_[ds_id].size());
  }

  // Most likely more events in the buffer. Though we might be exactly on the
  // boundary due to |max_samples|.
  return true;
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
          weak_this->DescriptorLookupTimeout(ds_id, pid);
      },
      timeout_ms);
}

void PerfProducer::DescriptorLookupTimeout(DataSourceInstanceID ds_id,
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
    PERFETTO_DLOG("TickDataSourceUnwind(%zu): source gone",
                  static_cast<size_t>(ds_id));
    return;
  }
  auto unwind_it = unwind_queues_.find(ds_id);
  PERFETTO_CHECK(unwind_it != unwind_queues_.end());

  PERFETTO_METATRACE_SCOPED(TAG_PRODUCER, PROFILER_UNWIND_TICK);

  bool queue_active =
      ProcessUnwindQueue(ds_id, &unwind_it->second, &ds_it->second);

  auto weak_this = weak_factory_.GetWeakPtr();
  if (!queue_active) {
    // Done with unwindings, push the source teardown to the end of the task
    // queue (to still process enqueued sampled).
    // TODO(rsavitski): under a dedicated unwinder thread, teardown of unwinding
    // state will happen here.
    task_runner_->PostTask([weak_this, ds_id] {
      if (weak_this)
        weak_this->FinishDataSourceStop(ds_id);
    });
  } else {
    // Otherwise, keep unwinding.
    task_runner_->PostDelayedTask(
        [weak_this, ds_id] {
          if (weak_this)
            weak_this->TickDataSourceUnwind(ds_id);
        },
        kUnwindTickPeriodMs - (NowMs() % kUnwindTickPeriodMs));
  }
}

// TODO(rsavitski): if we want to put a bound on the queue size (not as a
// function of proc-fd timeout), then the reader could purge kResolving entries
// from the start beyond that threshold.
// TODO(rsavitski): DataSource input won't be needed once fd-tracking in the
// unwinder is separated from fd-tracking in the reading frontend.
bool PerfProducer::ProcessUnwindQueue(DataSourceInstanceID ds_id,
                                      std::deque<UnwindEntry>* input_queue,
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
      PostEmitSkippedSample(ds_id, ProfilerStage::kUnwind,
                            std::move(entry.sample));
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
      PERFETTO_METATRACE_SCOPED(TAG_PRODUCER, PROFILER_UNWIND_SAMPLE);

      PerfProducer::CompletedSample unwound_sample =
          UnwindSample(std::move(sample), &proc_fd_it->second);

      PostEmitSample(ds_id, std::move(unwound_sample));
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

  // Metatrace: counter sensible only when there's a single active source.
  PERFETTO_METATRACE_COUNTER(TAG_PRODUCER, PROFILER_UNWIND_QUEUE_SZ,
                             queue.size());
  PERFETTO_DLOG("Unwind queue drain: [%zu]->[%zu]", num_samples, queue.size());

  // Return whether we're done with unwindings for this source.
  if (PERFETTO_UNLIKELY(ds.unwind_stopping) && queue.empty()) {
    return false;
  }
  return true;
}

PerfProducer::CompletedSample PerfProducer::UnwindSample(
    ParsedSample sample,
    DataSource::ProcDescriptors* process_state) {
  PerfProducer::CompletedSample ret;
  ret.cpu = sample.cpu;
  ret.pid = sample.pid;
  ret.tid = sample.tid;
  ret.timestamp = sample.timestamp;
  ret.cpu_mode = sample.cpu_mode;

  auto& unwind_state = process_state->unwind_state;

  // Overlay the stack bytes over /proc/<pid>/mem.
  std::shared_ptr<unwindstack::Memory> overlay_memory =
      std::make_shared<StackOverlayMemory>(
          unwind_state.fd_mem, sample.regs->sp(),
          reinterpret_cast<uint8_t*>(sample.stack.data()), sample.stack.size());

  // Unwindstack clobbers registers, so make a copy in case we need to retry.
  auto working_regs = std::unique_ptr<unwindstack::Regs>{sample.regs->Clone()};

  unwindstack::ErrorCode error_code = unwindstack::ERROR_NONE;
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

  PERFETTO_DLOG("Frames from unwindstack:");
  std::vector<unwindstack::FrameData> frames = unwinder.ConsumeFrames();
  for (unwindstack::FrameData& frame : frames) {
    if (PERFETTO_DLOG_IS_ON())
      PERFETTO_DLOG("%s", unwinder.FormatFrame(frame).c_str());

    ret.frames.emplace_back(unwind_state.AnnotateFrame(std::move(frame)));
  }

  // In case of an unwinding error, add a synthetic error frame (which will
  // appear as a caller of the partially-unwound fragment), for easier
  // visualization of errors.
  if (error_code != unwindstack::ERROR_NONE) {
    PERFETTO_DLOG("Unwinding error %" PRIu8, error_code);
    unwindstack::FrameData frame_data{};
    frame_data.function_name = "ERROR " + std::to_string(error_code);
    frame_data.map_name = "ERROR";
    ret.frames.emplace_back(std::move(frame_data), /*build_id=*/"");
    ret.unwind_error = error_code;
  }

  return ret;
}

void PerfProducer::PostEmitSample(DataSourceInstanceID ds_id,
                                  CompletedSample sample) {
  // hack: c++11 lambdas can't be moved into, so stash the sample on the heap.
  CompletedSample* raw_sample = new CompletedSample(std::move(sample));
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_this, ds_id, raw_sample] {
    if (weak_this)
      weak_this->EmitSample(ds_id, std::move(*raw_sample));
    delete raw_sample;
  });
}

void PerfProducer::EmitSample(DataSourceInstanceID ds_id,
                              CompletedSample sample) {
  auto ds_it = data_sources_.find(ds_id);
  if (ds_it == data_sources_.end()) {
    PERFETTO_DLOG("EmitSample(%zu): source gone", static_cast<size_t>(ds_id));
    return;
  }
  DataSource& ds = ds_it->second;

  // intern callsite
  GlobalCallstackTrie::Node* callstack_root =
      callstack_trie_.CreateCallsite(sample.frames);
  uint64_t callstack_iid = callstack_root->id();

  // start packet
  auto packet = ds.trace_writer->NewTracePacket();
  packet->set_timestamp(sample.timestamp);

  // write new interning data (if any)
  protos::pbzero::InternedData* interned_out = packet->set_interned_data();
  ds.interning_output.WriteCallstack(callstack_root, &callstack_trie_,
                                     interned_out);

  // write the sample itself
  auto* perf_sample = packet->set_perf_sample();
  perf_sample->set_cpu(sample.cpu);
  perf_sample->set_pid(static_cast<uint32_t>(sample.pid));
  perf_sample->set_tid(static_cast<uint32_t>(sample.tid));
  perf_sample->set_cpu_mode(ToCpuModeEnum(sample.cpu_mode));
  perf_sample->set_callstack_iid(callstack_iid);
  if (sample.unwind_error != unwindstack::ERROR_NONE) {
    perf_sample->set_unwind_error(ToProtoEnum(sample.unwind_error));
  }
}

void PerfProducer::EmitRingBufferLoss(DataSourceInstanceID ds_id,
                                      size_t cpu,
                                      uint64_t records_lost) {
  auto ds_it = data_sources_.find(ds_id);
  if (ds_it == data_sources_.end()) {
    PERFETTO_DLOG("EmitRingBufferLoss(%zu): source gone",
                  static_cast<size_t>(ds_id));
    return;
  }
  DataSource& ds = ds_it->second;
  PERFETTO_DLOG("DataSource(%zu): cpu%zu lost [%" PRIu64 "] records",
                static_cast<size_t>(ds_id), cpu, records_lost);

  // The data loss record relates to a single ring buffer, and indicates loss
  // since the last successfully-written record in that buffer. Therefore the
  // data loss record itself has no timestamp.
  // We timestamp the packet with the boot clock for packet ordering purposes,
  // but it no longer has a (precise) interpretation relative to the sample
  // stream from that per-cpu buffer. See the proto comments for more details.
  auto packet = ds.trace_writer->NewTracePacket();
  packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));

  auto* perf_sample = packet->set_perf_sample();
  perf_sample->set_cpu(static_cast<uint32_t>(cpu));
  perf_sample->set_kernel_records_lost(records_lost);
}

void PerfProducer::PostEmitSkippedSample(DataSourceInstanceID ds_id,
                                         ProfilerStage stage,
                                         ParsedSample sample) {
  // hack: c++11 lambdas can't be moved into, so stash the sample on the heap.
  ParsedSample* raw_sample = new ParsedSample(std::move(sample));
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_this, ds_id, stage, raw_sample] {
    if (weak_this)
      weak_this->EmitSkippedSample(ds_id, stage, std::move(*raw_sample));
    delete raw_sample;
  });
}

void PerfProducer::EmitSkippedSample(DataSourceInstanceID ds_id,
                                     ProfilerStage stage,
                                     ParsedSample sample) {
  auto ds_it = data_sources_.find(ds_id);
  if (ds_it == data_sources_.end()) {
    PERFETTO_DLOG("EmitSkippedSample(%zu): source gone",
                  static_cast<size_t>(ds_id));
    return;
  }
  DataSource& ds = ds_it->second;

  auto packet = ds.trace_writer->NewTracePacket();
  packet->set_timestamp(sample.timestamp);
  auto* perf_sample = packet->set_perf_sample();
  perf_sample->set_cpu(sample.cpu);
  perf_sample->set_pid(static_cast<uint32_t>(sample.pid));
  perf_sample->set_tid(static_cast<uint32_t>(sample.tid));
  perf_sample->set_cpu_mode(ToCpuModeEnum(sample.cpu_mode));

  using PerfSample = protos::pbzero::PerfSample;
  switch (stage) {
    case ProfilerStage::kRead:
      perf_sample->set_sample_skipped_reason(PerfSample::PROFILER_STAGE_READ);
      break;
    case ProfilerStage::kUnwind:
      perf_sample->set_sample_skipped_reason(PerfSample::PROFILER_STAGE_UNWIND);
      break;
  }
}

void PerfProducer::InitiateReaderStop(DataSource* ds) {
  PERFETTO_DLOG("InitiateReaderStop");
  ds->reader_stopping = true;
  for (auto& event_reader : ds->per_cpu_readers) {
    event_reader.PauseEvents();
  }
}

void PerfProducer::InitiateUnwindStop(DataSource* ds) {
  PERFETTO_DLOG("InitiateUnwindStop");
  PERFETTO_CHECK(ds->reader_stopping);
  ds->unwind_stopping = true;
}

void PerfProducer::FinishDataSourceStop(DataSourceInstanceID ds_id) {
  PERFETTO_DLOG("FinishDataSourceStop(%zu)", static_cast<size_t>(ds_id));
  auto ds_it = data_sources_.find(ds_id);
  PERFETTO_CHECK(ds_it != data_sources_.end());
  DataSource& ds = ds_it->second;

  PERFETTO_CHECK(ds.reader_stopping);
  PERFETTO_CHECK(ds.unwind_stopping);

  ds.trace_writer->Flush();
  data_sources_.erase(ds_id);
  unwind_queues_.erase(ds_id);

  endpoint_->NotifyDataSourceStopped(ds_id);

  // Clean up resources if there are no more active sources.
  if (data_sources_.empty()) {
    // purge internings
    callstack_trie_.ClearTrie();
    // clear and re-enable libunwindstack's cache
    unwindstack::Elf::SetCachingEnabled(false);
    unwindstack::Elf::SetCachingEnabled(true);
  }
}

void PerfProducer::StartMetatraceSource(DataSourceInstanceID ds_id,
                                        BufferID target_buffer) {
  auto writer = endpoint_->CreateTraceWriter(target_buffer);

  auto it_and_inserted = metatrace_writers_.emplace(
      std::piecewise_construct, std::make_tuple(ds_id), std::make_tuple());
  PERFETTO_DCHECK(it_and_inserted.second);
  // Note: only the first concurrent writer will actually be active.
  metatrace_writers_[ds_id].Enable(task_runner_, std::move(writer),
                                   metatrace::TAG_ANY);
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
  endpoint_ = ProducerIPCClient::Connect(
      producer_socket_name_, this, kProducerName, task_runner_,
      TracingService::ProducerSMBScrapingMode::kEnabled);
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

  {
    // linux.perf
    DataSourceDescriptor desc;
    desc.set_name(kDataSourceName);
    desc.set_will_notify_on_stop(true);
    endpoint_->RegisterDataSource(desc);
  }
  {
    // metatrace
    DataSourceDescriptor desc;
    desc.set_name(MetatraceWriter::kDataSourceName);
    endpoint_->RegisterDataSource(desc);
  }
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
