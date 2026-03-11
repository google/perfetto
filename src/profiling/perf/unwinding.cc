/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/profiling/perf/unwinding.h"

#include <cinttypes>
#include <mutex>

#include "perfetto/ext/base/metatrace.h"
#include "perfetto/ext/base/thread_utils.h"
#include "perfetto/ext/base/utils.h"

namespace {
constexpr size_t kUnwindingMaxFrames = 1000;
constexpr uint32_t kDataSourceShutdownRetryDelayMs = 400;
}  // namespace

namespace perfetto {
namespace profiling {

Unwinder::Delegate::~Delegate() = default;

Unwinder::Unwinder(Delegate* delegate,
                   base::MaybeLockFreeTaskRunner* task_runner,
                   UnwindBackend* backend)
    : backend_(backend), task_runner_(task_runner), delegate_(delegate) {
  backend_->ResetCache();
  base::MaybeSetThreadName("stack-unwinding");
}

void Unwinder::PostStartDataSource(DataSourceInstanceID ds_id,
                                   bool kernel_frames,
                                   UnwindMode unwind_mode) {
  // No need for a weak pointer as the associated task runner quits (stops
  // running tasks) strictly before the Unwinder's destruction.
  task_runner_->PostTask([this, ds_id, kernel_frames, unwind_mode] {
    StartDataSource(ds_id, kernel_frames, unwind_mode);
  });
}

void Unwinder::StartDataSource(DataSourceInstanceID ds_id,
                               bool kernel_frames,
                               UnwindMode unwind_mode) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Unwinder::StartDataSource(%zu)", static_cast<size_t>(ds_id));

  auto it_and_inserted =
      data_sources_.emplace(ds_id, DataSourceState{unwind_mode});
  PERFETTO_DCHECK(it_and_inserted.second);

  if (kernel_frames) {
    kernel_symbolizer_.GetOrCreateKernelSymbolMap();
  }
}

// c++11: use shared_ptr to transfer resource handles, so that the resources get
// released even if the task runner is destroyed with pending tasks.
void Unwinder::PostAdoptProcDescriptors(DataSourceInstanceID ds_id,
                                        pid_t pid,
                                        base::ScopedFile maps_fd,
                                        base::ScopedFile mem_fd) {
  auto shared_maps = std::make_shared<base::ScopedFile>(std::move(maps_fd));
  auto shared_mem = std::make_shared<base::ScopedFile>(std::move(mem_fd));
  task_runner_->PostTask([this, ds_id, pid, shared_maps, shared_mem] {
    base::ScopedFile maps = std::move(*shared_maps.get());
    base::ScopedFile mem = std::move(*shared_mem.get());
    AdoptProcDescriptors(ds_id, pid, std::move(maps), std::move(mem));
  });
}

void Unwinder::AdoptProcDescriptors(DataSourceInstanceID ds_id,
                                    pid_t pid,
                                    base::ScopedFile maps_fd,
                                    base::ScopedFile mem_fd) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Unwinder::AdoptProcDescriptors(%zu, %d, %d, %d)",
                static_cast<size_t>(ds_id), static_cast<int>(pid),
                maps_fd.get(), mem_fd.get());

  auto it = data_sources_.find(ds_id);
  if (it == data_sources_.end())
    return;
  DataSourceState& ds = it->second;

  ProcessState& proc_state = ds.process_states[pid];  // insert if new
  PERFETTO_DCHECK(proc_state.status == ProcessState::Status::kInitial ||
                  proc_state.status == ProcessState::Status::kFdsTimedOut);
  PERFETTO_DCHECK(!proc_state.unwind_state);

  PERFETTO_METATRACE_SCOPED(TAG_PRODUCER, PROFILER_MAPS_PARSE);

  proc_state.status = ProcessState::Status::kFdsResolved;
  proc_state.unwind_state =
      backend_->CreateProcessState(std::move(maps_fd), std::move(mem_fd));
}

void Unwinder::PostRecordTimedOutProcDescriptors(DataSourceInstanceID ds_id,
                                                 pid_t pid) {
  task_runner_->PostTask([this, ds_id, pid] {
    UpdateProcessStateStatus(ds_id, pid, ProcessState::Status::kFdsTimedOut);
  });
}

void Unwinder::PostRecordNoUserspaceProcess(DataSourceInstanceID ds_id,
                                            pid_t pid) {
  task_runner_->PostTask([this, ds_id, pid] {
    UpdateProcessStateStatus(ds_id, pid, ProcessState::Status::kNoUserspace);
  });
}

void Unwinder::UpdateProcessStateStatus(DataSourceInstanceID ds_id,
                                        pid_t pid,
                                        ProcessState::Status new_status) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Unwinder::UpdateProcessStateStatus(%zu, %d, %d)",
                static_cast<size_t>(ds_id), static_cast<int>(pid),
                static_cast<int>(new_status));

  auto it = data_sources_.find(ds_id);
  if (it == data_sources_.end())
    return;
  DataSourceState& ds = it->second;

  ProcessState& proc_state = ds.process_states[pid];  // insert if new
  proc_state.status = new_status;
}

void Unwinder::PostProcessQueue() {
  task_runner_->PostTask([this] { ProcessQueue(); });
}

// Note: we always walk the queue in order. So if there are multiple data
// sources, one of which is shutting down, its shutdown can be delayed by
// unwinding of other sources' samples.
void Unwinder::ProcessQueue() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_METATRACE_SCOPED(TAG_PRODUCER, PROFILER_UNWIND_TICK);
  PERFETTO_DLOG("Unwinder::ProcessQueue");

  base::FlatSet<DataSourceInstanceID> pending_sample_sources =
      ConsumeAndUnwindReadySamples();

  // Deal with the possibility of data sources that are shutting down.
  bool post_delayed_reprocess = false;
  base::FlatSet<DataSourceInstanceID> sources_to_stop;
  for (auto& id_and_ds : data_sources_) {
    DataSourceInstanceID ds_id = id_and_ds.first;
    const DataSourceState& ds = id_and_ds.second;

    if (ds.status == DataSourceState::Status::kActive)
      continue;

    if (pending_sample_sources.count(ds_id)) {
      PERFETTO_DLOG(
          "Unwinder delaying DS(%zu) stop: waiting on a pending sample",
          static_cast<size_t>(ds_id));
      post_delayed_reprocess = true;
    } else {
      sources_to_stop.insert(ds_id);
    }
  }

  for (auto ds_id : sources_to_stop)
    FinishDataSourceStop(ds_id);

  if (post_delayed_reprocess)
    task_runner_->PostDelayedTask([this] { ProcessQueue(); },
                                  kDataSourceShutdownRetryDelayMs);
}

base::FlatSet<DataSourceInstanceID> Unwinder::ConsumeAndUnwindReadySamples() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  base::FlatSet<DataSourceInstanceID> pending_sample_sources;

  // Use a single snapshot of the ring buffer pointers.
  ReadView read_view = unwind_queue_.BeginRead();

  PERFETTO_METATRACE_COUNTER(
      TAG_PRODUCER, PROFILER_UNWIND_QUEUE_SZ,
      static_cast<int32_t>(read_view.write_pos - read_view.read_pos));

  if (read_view.read_pos == read_view.write_pos)
    return pending_sample_sources;

  // Walk the queue.
  for (auto read_pos = read_view.read_pos; read_pos < read_view.write_pos;
       read_pos++) {
    UnwindEntry& entry = unwind_queue_.at(read_pos);

    if (!entry.valid)
      continue;  // already processed

    uint64_t sampled_stack_bytes = entry.sample.stack.size();

    // Data source might be gone due to an abrupt stop.
    auto it = data_sources_.find(entry.data_source_id);
    if (it == data_sources_.end()) {
      entry = UnwindEntry::Invalid();
      DecrementEnqueuedFootprint(sampled_stack_bytes);
      continue;
    }
    DataSourceState& ds = it->second;

    pid_t pid = entry.sample.common.pid;
    ProcessState& proc_state = ds.process_states[pid];  // insert if new

    // Giving up on the sample (proc-fd lookup timed out).
    if (proc_state.status == ProcessState::Status::kFdsTimedOut) {
      PERFETTO_DLOG("Unwinder skipping sample for pid [%d]: kFdsTimedOut",
                    static_cast<int>(pid));

      entry.sample.stack.clear();
      entry.sample.stack.shrink_to_fit();

      delegate_->PostEmitUnwinderSkippedSample(entry.data_source_id,
                                               std::move(entry.sample));
      entry = UnwindEntry::Invalid();
      DecrementEnqueuedFootprint(sampled_stack_bytes);
      continue;
    }

    // Still waiting to be notified how to handle this process.
    if (proc_state.status == ProcessState::Status::kInitial) {
      PERFETTO_DLOG("Unwinder deferring sample for pid [%d]",
                    static_cast<int>(pid));

      pending_sample_sources.insert(entry.data_source_id);
      continue;
    }

    // b/324757089: kernel thread reusing a userspace pid.
    if (PERFETTO_UNLIKELY(!entry.sample.regs &&
                          proc_state.status ==
                              ProcessState::Status::kFdsResolved)) {
      PERFETTO_DLOG(
          "Unwinder discarding sample for pid [%d]: uspace->kthread pid reuse",
          static_cast<int>(pid));

      PERFETTO_CHECK(sampled_stack_bytes == 0);
      delegate_->PostEmitUnwinderSkippedSample(entry.data_source_id,
                                               std::move(entry.sample));
      entry = UnwindEntry::Invalid();
      continue;
    }

    // Sample ready - process it.
    if (proc_state.status == ProcessState::Status::kFdsResolved ||
        proc_state.status == ProcessState::Status::kNoUserspace) {
      PERFETTO_METATRACE_SCOPED(TAG_PRODUCER, PROFILER_UNWIND_SAMPLE);
      PERFETTO_METATRACE_COUNTER(TAG_PRODUCER, PROFILER_UNWIND_CURRENT_PID,
                                 static_cast<int32_t>(pid));

      PERFETTO_CHECK(proc_state.status == ProcessState::Status::kNoUserspace ||
                     proc_state.unwind_state != nullptr);

      ProcessUnwindState* opt_user_state = proc_state.unwind_state.get();
      CompletedSample unwound_sample =
          UnwindSample(entry.sample, opt_user_state,
                       proc_state.attempted_unwinding, ds.unwind_mode);
      proc_state.attempted_unwinding = true;

      PERFETTO_METATRACE_COUNTER(TAG_PRODUCER, PROFILER_UNWIND_CURRENT_PID, 0);

      delegate_->PostEmitSample(entry.data_source_id,
                                std::move(unwound_sample));
      entry = UnwindEntry::Invalid();
      DecrementEnqueuedFootprint(sampled_stack_bytes);
      continue;
    }
  }

  // Consume all leading processed entries in the queue.
  auto new_read_pos = read_view.read_pos;
  for (; new_read_pos < read_view.write_pos; new_read_pos++) {
    UnwindEntry& entry = unwind_queue_.at(new_read_pos);
    if (entry.valid)
      break;
  }
  if (new_read_pos != read_view.read_pos)
    unwind_queue_.CommitNewReadPosition(new_read_pos);

  PERFETTO_METATRACE_COUNTER(
      TAG_PRODUCER, PROFILER_UNWIND_QUEUE_SZ,
      static_cast<int32_t>(read_view.write_pos - new_read_pos));

  PERFETTO_DLOG("Unwind queue drain: [%" PRIu64 "]->[%" PRIu64 "]",
                read_view.write_pos - read_view.read_pos,
                read_view.write_pos - new_read_pos);

  return pending_sample_sources;
}

CompletedSample Unwinder::UnwindSample(const ParsedSample& sample,
                                       ProcessUnwindState* opt_user_state,
                                       bool pid_unwound_before,
                                       UnwindMode unwind_mode) {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  CompletedSample ret;
  ret.common = sample.common;

  // Symbolize kernel-unwound kernel frames, if appropriate.
  std::vector<UnwindFrame> kernel_frames = SymbolizeKernelCallchain(sample);

  size_t kernel_frames_size = kernel_frames.size();
  ret.frames = std::move(kernel_frames);

  // Perform userspace unwinding, if appropriate.
  if (!opt_user_state)
    return ret;

  PERFETTO_DCHECK(sample.regs.has_value());

  auto attempt_unwind = [&]() -> UnwindBackend::UnwindResult {
    metatrace::ScopedEvent m(metatrace::TAG_PRODUCER,
                             pid_unwound_before
                                 ? metatrace::PROFILER_UNWIND_ATTEMPT
                                 : metatrace::PROFILER_UNWIND_INITIAL_ATTEMPT);

    switch (unwind_mode) {
      case UnwindMode::kFramePointer:
        return backend_->UnwindFramePointers(
            opt_user_state, sample.regs.value(), sample.regs.value().sp,
            sample.stack.data(), sample.stack.size(), kUnwindingMaxFrames);
      case UnwindMode::kUnwindStack:
        return backend_->Unwind(opt_user_state, sample.regs.value(),
                                sample.regs.value().sp, sample.stack.data(),
                                sample.stack.size(), kUnwindingMaxFrames);
    }
  };

  // first unwind attempt
  UnwindBackend::UnwindResult unwind = attempt_unwind();

  bool should_retry = unwind.error_code == UnwindErrorCode::kInvalidMap ||
                      (unwind.warnings & 1);  // WARNING_DEX_PC_NOT_IN_MAP

  // ERROR_INVALID_MAP means that unwinding reached a point in memory without a
  // corresponding mapping. This is possible if the parsed /proc/pid/maps is
  // outdated. Reparse and try again.
  //
  // Special case: skip reparsing if the stack sample was (most likely)
  // truncated.
  if (should_retry && sample.stack_maxed) {
    PERFETTO_DLOG("Skipping reparse/reunwind due to maxed stack for tid [%d]",
                  static_cast<int>(sample.common.tid));
  } else if (should_retry) {
    {
      PERFETTO_METATRACE_SCOPED(TAG_PRODUCER, PROFILER_MAPS_REPARSE);
      PERFETTO_DLOG("Reparsing maps for pid [%d]",
                    static_cast<int>(sample.common.pid));
      backend_->ReparseMaps(opt_user_state);
    }
    // reunwind attempt
    unwind = attempt_unwind();
  }

  ret.frames.reserve(kernel_frames_size + unwind.frames.size());
  for (UnwindFrame& frame : unwind.frames) {
    ret.frames.emplace_back(std::move(frame));
  }

  // In case of an unwinding error, add a synthetic error frame.
  if (unwind.error_code != UnwindErrorCode::kNone) {
    PERFETTO_DLOG("Unwinding error %" PRIu8,
                  static_cast<uint8_t>(unwind.error_code));
    UnwindFrame frame{};
    frame.function_name = "ERROR " + StringifyUnwindError(unwind.error_code);
    ret.frames.emplace_back(std::move(frame));
    ret.unwind_error = unwind.error_code;
  }

  return ret;
}

std::vector<UnwindFrame> Unwinder::SymbolizeKernelCallchain(
    const ParsedSample& sample) {
  std::vector<UnwindFrame> ret;
  if (sample.kernel_ips.empty())
    return ret;

  if (sample.kernel_ips[0] != PERF_CONTEXT_KERNEL) {
    PERFETTO_DFATAL_OR_ELOG(
        "Unexpected: 0th frame of callchain is not PERF_CONTEXT_KERNEL.");
    return ret;
  }

  auto* kernel_map = kernel_symbolizer_.GetOrCreateKernelSymbolMap();
  PERFETTO_DCHECK(kernel_map);
  ret.reserve(sample.kernel_ips.size());
  for (size_t i = 1; i < sample.kernel_ips.size(); i++) {
    std::string function_name = kernel_map->Lookup(sample.kernel_ips[i]);

    UnwindFrame frame{};
    frame.function_name = std::move(function_name);
    frame.map_name = "kernel";
    ret.emplace_back(std::move(frame));
  }
  return ret;
}

void Unwinder::PostInitiateDataSourceStop(DataSourceInstanceID ds_id) {
  task_runner_->PostTask([this, ds_id] { InitiateDataSourceStop(ds_id); });
}

void Unwinder::InitiateDataSourceStop(DataSourceInstanceID ds_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Unwinder::InitiateDataSourceStop(%zu)",
                static_cast<size_t>(ds_id));

  auto it = data_sources_.find(ds_id);
  if (it == data_sources_.end())
    return;
  DataSourceState& ds = it->second;

  PERFETTO_CHECK(ds.status == DataSourceState::Status::kActive);
  ds.status = DataSourceState::Status::kShuttingDown;

  PostProcessQueue();
}

void Unwinder::FinishDataSourceStop(DataSourceInstanceID ds_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Unwinder::FinishDataSourceStop(%zu)",
                static_cast<size_t>(ds_id));

  auto it = data_sources_.find(ds_id);
  if (it == data_sources_.end())
    return;
  DataSourceState& ds = it->second;

  PERFETTO_CHECK(ds.status == DataSourceState::Status::kShuttingDown);
  data_sources_.erase(it);

  if (data_sources_.empty()) {
    kernel_symbolizer_.Destroy();
    backend_->ResetCache();
  }

  delegate_->PostFinishDataSourceStop(ds_id);
}

void Unwinder::PostPurgeDataSource(DataSourceInstanceID ds_id) {
  task_runner_->PostTask([this, ds_id] { PurgeDataSource(ds_id); });
}

void Unwinder::PurgeDataSource(DataSourceInstanceID ds_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Unwinder::PurgeDataSource(%zu)", static_cast<size_t>(ds_id));

  auto it = data_sources_.find(ds_id);
  if (it == data_sources_.end())
    return;

  data_sources_.erase(it);

  if (data_sources_.empty()) {
    kernel_symbolizer_.Destroy();
    backend_->ResetCache();
    base::MaybeReleaseAllocatorMemToOS();
  }
}

void Unwinder::PostClearCachedStatePeriodic(DataSourceInstanceID ds_id,
                                            uint32_t period_ms) {
  task_runner_->PostDelayedTask(
      [this, ds_id, period_ms] { ClearCachedStatePeriodic(ds_id, period_ms); },
      period_ms);
}

void Unwinder::ClearCachedStatePeriodic(DataSourceInstanceID ds_id,
                                        uint32_t period_ms) {
  auto it = data_sources_.find(ds_id);
  if (it == data_sources_.end())
    return;

  DataSourceState& ds = it->second;
  if (ds.status != DataSourceState::Status::kActive)
    return;

  PERFETTO_METATRACE_SCOPED(TAG_PRODUCER, PROFILER_UNWIND_CACHE_CLEAR);
  PERFETTO_DLOG("Clearing unwinder's cached state.");

  for (auto& pid_and_process : ds.process_states) {
    if (pid_and_process.second.status == ProcessState::Status::kFdsResolved)
      backend_->ReparseMaps(pid_and_process.second.unwind_state.get());
  }
  backend_->ResetCache();
  base::MaybeReleaseAllocatorMemToOS();

  PostClearCachedStatePeriodic(ds_id, period_ms);  // repost
}

}  // namespace profiling
}  // namespace perfetto
