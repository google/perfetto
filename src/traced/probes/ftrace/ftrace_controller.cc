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

#include "src/traced/probes/ftrace/ftrace_controller.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <string>
#include <utility>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/base/utils.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "src/traced/probes/ftrace/cpu_reader.h"
#include "src/traced/probes/ftrace/cpu_stats_parser.h"
#include "src/traced/probes/ftrace/event_info.h"
#include "src/traced/probes/ftrace/ftrace_config_muxer.h"
#include "src/traced/probes/ftrace/ftrace_data_source.h"
#include "src/traced/probes/ftrace/ftrace_metadata.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "src/traced/probes/ftrace/ftrace_stats.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"

namespace perfetto {
namespace {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
constexpr const char* kTracingPaths[] = {
    "/sys/kernel/tracing/", "/sys/kernel/debug/tracing/", nullptr,
};
#else
constexpr const char* kTracingPaths[] = {
    "/sys/kernel/debug/tracing/", nullptr,
};
#endif

constexpr int kDefaultDrainPeriodMs = 100;
constexpr int kMinDrainPeriodMs = 1;
constexpr int kMaxDrainPeriodMs = 1000 * 60;

uint32_t ClampDrainPeriodMs(uint32_t drain_period_ms) {
  if (drain_period_ms == 0) {
    return kDefaultDrainPeriodMs;
  }
  if (drain_period_ms < kMinDrainPeriodMs ||
      kMaxDrainPeriodMs < drain_period_ms) {
    PERFETTO_LOG("drain_period_ms was %u should be between %u and %u",
                 drain_period_ms, kMinDrainPeriodMs, kMaxDrainPeriodMs);
    return kDefaultDrainPeriodMs;
  }
  return drain_period_ms;
}

void WriteToFile(const char* path, const char* str) {
  int fd = open(path, O_WRONLY);
  if (fd == -1)
    return;
  perfetto::base::ignore_result(write(fd, str, strlen(str)));
  perfetto::base::ignore_result(close(fd));
}

void ClearFile(const char* path) {
  int fd = open(path, O_WRONLY | O_TRUNC);
  if (fd == -1)
    return;
  perfetto::base::ignore_result(close(fd));
}

}  // namespace

// Method of last resort to reset ftrace state.
// We don't know what state the rest of the system and process is so as far
// as possible avoid allocations.
void HardResetFtraceState() {
  WriteToFile("/sys/kernel/debug/tracing/tracing_on", "0");
  WriteToFile("/sys/kernel/debug/tracing/buffer_size_kb", "4");
  WriteToFile("/sys/kernel/debug/tracing/events/enable", "0");
  ClearFile("/sys/kernel/debug/tracing/trace");

  WriteToFile("/sys/kernel/tracing/tracing_on", "0");
  WriteToFile("/sys/kernel/tracing/buffer_size_kb", "4");
  WriteToFile("/sys/kernel/tracing/events/enable", "0");
  ClearFile("/sys/kernel/tracing/trace");
}

// static
// TODO(taylori): Add a test for tracing paths in integration tests.
std::unique_ptr<FtraceController> FtraceController::Create(
    base::TaskRunner* runner,
    Observer* observer) {
  size_t index = 0;
  std::unique_ptr<FtraceProcfs> ftrace_procfs = nullptr;
  while (!ftrace_procfs && kTracingPaths[index]) {
    ftrace_procfs = FtraceProcfs::Create(kTracingPaths[index++]);
  }

  if (!ftrace_procfs)
    return nullptr;

  auto table = ProtoTranslationTable::Create(
      ftrace_procfs.get(), GetStaticEventInfo(), GetStaticCommonFieldsInfo());

  std::unique_ptr<FtraceConfigMuxer> model = std::unique_ptr<FtraceConfigMuxer>(
      new FtraceConfigMuxer(ftrace_procfs.get(), table.get()));
  return std::unique_ptr<FtraceController>(
      new FtraceController(std::move(ftrace_procfs), std::move(table),
                           std::move(model), runner, observer));
}

FtraceController::FtraceController(std::unique_ptr<FtraceProcfs> ftrace_procfs,
                                   std::unique_ptr<ProtoTranslationTable> table,
                                   std::unique_ptr<FtraceConfigMuxer> model,
                                   base::TaskRunner* task_runner,
                                   Observer* observer)
    : task_runner_(task_runner),
      observer_(observer),
      ftrace_procfs_(std::move(ftrace_procfs)),
      table_(std::move(table)),
      ftrace_config_muxer_(std::move(model)),
      weak_factory_(this) {}

FtraceController::~FtraceController() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  for (const auto* data_source : data_sources_)
    ftrace_config_muxer_->RemoveConfig(data_source->config_id());
  data_sources_.clear();
  StopIfNeeded();
}

uint64_t FtraceController::NowMs() const {
  return static_cast<uint64_t>(base::GetWallTimeMs().count());
}

// static
void FtraceController::DrainCPUs(base::WeakPtr<FtraceController> weak_this,
                                 size_t generation) {
  // The controller might be gone.
  FtraceController* ctrl = weak_this.get();
  if (!ctrl)
    return;

  // We might have stopped tracing then quickly re-enabled it, in this case
  // we don't want to end up with two periodic tasks for each CPU:
  if (ctrl->generation_ != generation)
    return;

  PERFETTO_DCHECK_THREAD(ctrl->thread_checker_);
  std::bitset<kMaxCpus> cpus_to_drain;
  {
    std::unique_lock<std::mutex> lock(ctrl->lock_);
    // We might have stopped caring about events.
    if (!ctrl->listening_for_raw_trace_data_)
      return;
    std::swap(cpus_to_drain, ctrl->cpus_to_drain_);
  }

  for (size_t cpu = 0; cpu < ctrl->ftrace_procfs_->NumberOfCpus(); cpu++) {
    if (!cpus_to_drain[cpu])
      continue;
    // This method reads the pipe and converts the raw ftrace data into
    // protobufs using the |data_source|'s TraceWriter.
    ctrl->cpu_readers_[cpu]->Drain(ctrl->data_sources_);
    ctrl->OnDrainCpuForTesting(cpu);
  }

  // If we filled up any SHM pages while draining the data, we will have posted
  // a task to notify traced about this. Only unblock the readers after this
  // notification is sent to make it less likely that they steal CPU time away
  // from traced.
  ctrl->task_runner_->PostTask(
      std::bind(&FtraceController::UnblockReaders, weak_this));

  ctrl->observer_->OnFtraceDataWrittenIntoDataSourceBuffers();
}

// static
void FtraceController::UnblockReaders(
    const base::WeakPtr<FtraceController>& weak_this) {
  FtraceController* ctrl = weak_this.get();
  if (!ctrl)
    return;
  // Unblock all waiting readers to start moving more data into their
  // respective staging pipes.
  ctrl->data_drained_.notify_all();
}

void FtraceController::StartIfNeeded() {
  if (data_sources_.size() > 1)
    return;
  PERFETTO_CHECK(!data_sources_.empty());
  {
    std::unique_lock<std::mutex> lock(lock_);
    PERFETTO_CHECK(!listening_for_raw_trace_data_);
    listening_for_raw_trace_data_ = true;
  }
  generation_++;
  base::WeakPtr<FtraceController> weak_this = weak_factory_.GetWeakPtr();
  for (size_t cpu = 0; cpu < ftrace_procfs_->NumberOfCpus(); cpu++) {
    cpu_readers_.emplace(
        cpu, std::unique_ptr<CpuReader>(new CpuReader(
                 table_.get(), cpu, ftrace_procfs_->OpenPipeForCpu(cpu),
                 std::bind(&FtraceController::OnDataAvailable, this, weak_this,
                           generation_, cpu, GetDrainPeriodMs()))));
  }
}

uint32_t FtraceController::GetDrainPeriodMs() {
  if (data_sources_.empty())
    return kDefaultDrainPeriodMs;
  uint32_t min_drain_period_ms = kMaxDrainPeriodMs + 1;
  for (const FtraceDataSource* data_source : data_sources_) {
    if (data_source->config().drain_period_ms() < min_drain_period_ms)
      min_drain_period_ms = data_source->config().drain_period_ms();
  }
  return ClampDrainPeriodMs(min_drain_period_ms);
}

void FtraceController::ClearTrace() {
  ftrace_procfs_->ClearTrace();
}

void FtraceController::DisableAllEvents() {
  ftrace_procfs_->DisableAllEvents();
}

void FtraceController::WriteTraceMarker(const std::string& s) {
  ftrace_procfs_->WriteTraceMarker(s);
}

void FtraceController::StopIfNeeded() {
  if (!data_sources_.empty())
    return;
  {
    // Unblock any readers that are waiting for us to drain data.
    std::unique_lock<std::mutex> lock(lock_);
    listening_for_raw_trace_data_ = false;
    cpus_to_drain_.reset();
  }
  data_drained_.notify_all();
  cpu_readers_.clear();
}

// This method is called on the worker thread. Lifetime is guaranteed to be
// valid, because the FtraceController dtor (that happens on the main thread)
// joins the worker threads. |weak_this| is passed and not derived, because the
// WeakPtrFactory is accessible only on the main thread.
void FtraceController::OnDataAvailable(
    base::WeakPtr<FtraceController> weak_this,
    size_t generation,
    size_t cpu,
    uint32_t drain_period_ms) {
  PERFETTO_DCHECK(cpu < ftrace_procfs_->NumberOfCpus());
  std::unique_lock<std::mutex> lock(lock_);
  if (!listening_for_raw_trace_data_)
    return;
  if (cpus_to_drain_.none()) {
    // If this was the first CPU to wake up, schedule a drain for the next drain
    // interval.
    uint32_t delay_ms = drain_period_ms - (NowMs() % drain_period_ms);
    task_runner_->PostDelayedTask(
        std::bind(&FtraceController::DrainCPUs, weak_this, generation),
        delay_ms);
  }
  cpus_to_drain_[cpu] = true;

  // Wait until the main thread has finished draining.
  // TODO(skyostil): The threads waiting here will all try to grab lock_
  // when woken up. Find a way to avoid this.
  data_drained_.wait(lock, [this, cpu] {
    return !cpus_to_drain_[cpu] || !listening_for_raw_trace_data_;
  });
}

bool FtraceController::AddDataSource(FtraceDataSource* data_source) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!ValidConfig(data_source->config()))
    return false;

  auto config_id = ftrace_config_muxer_->RequestConfig(data_source->config());
  if (!config_id)
    return false;

  std::unique_ptr<EventFilter> filter(new EventFilter(
      *table_, FtraceEventsAsSet(*ftrace_config_muxer_->GetConfig(config_id))));
  auto it_and_inserted = data_sources_.insert(data_source);
  PERFETTO_DCHECK(it_and_inserted.second);
  StartIfNeeded();
  data_source->Initialize(config_id, std::move(filter));
  return true;
}

void FtraceController::RemoveDataSource(FtraceDataSource* data_source) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  size_t removed = data_sources_.erase(data_source);
  if (!removed)
    return;  // Can happen if AddDataSource failed (e.g. too many sessions).
  ftrace_config_muxer_->RemoveConfig(data_source->config_id());
  StopIfNeeded();
}

void FtraceController::DumpFtraceStats(FtraceStats* stats) {
  DumpAllCpuStats(ftrace_procfs_.get(), stats);
}

FtraceController::Observer::~Observer() = default;

}  // namespace perfetto
