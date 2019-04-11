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
#include "perfetto/base/file_utils.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/metatrace.h"
#include "perfetto/base/time.h"
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

constexpr int kDefaultDrainPeriodMs = 100;
constexpr int kFlushTimeoutMs = 500;
constexpr int kMinDrainPeriodMs = 1;
constexpr int kMaxDrainPeriodMs = 1000 * 60;
constexpr uint32_t kMainThread = 255;  // for METATRACE

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
  auto fd = base::OpenFile(path, O_WRONLY);
  if (!fd)
    return;
  base::ignore_result(base::WriteAll(*fd, str, strlen(str)));
}

void ClearFile(const char* path) {
  auto fd = base::OpenFile(path, O_WRONLY | O_TRUNC);
}

}  // namespace

const char* const FtraceController::kTracingPaths[] = {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
    "/sys/kernel/tracing/", "/sys/kernel/debug/tracing/", nullptr,
#else
    "/sys/kernel/debug/tracing/", nullptr,
#endif
};

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

  if (!table)
    return nullptr;

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
      thread_sync_(task_runner),
      ftrace_procfs_(std::move(ftrace_procfs)),
      table_(std::move(table)),
      ftrace_config_muxer_(std::move(model)),
      weak_factory_(this) {
  thread_sync_.trace_controller_weak = GetWeakPtr();
}

FtraceController::~FtraceController() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  for (const auto* data_source : data_sources_)
    ftrace_config_muxer_->RemoveConfig(data_source->config_id());
  data_sources_.clear();
  started_data_sources_.clear();
  StopIfNeeded();
}

uint64_t FtraceController::NowMs() const {
  return static_cast<uint64_t>(base::GetWallTimeMs().count());
}

// The OnCpuReader* methods below are called on the CpuReader worker threads.
// Lifetime is guaranteed to be valid, because the FtraceController dtor
// (that happens on the main thread) joins the worker threads.

// static
void FtraceController::OnCpuReaderRead(size_t cpu,
                                       int generation,
                                       FtraceThreadSync* thread_sync) {
  PERFETTO_METATRACE("OnCpuReaderRead()", cpu);

  {
    std::lock_guard<std::mutex> lock(thread_sync->mutex);
    // If this was the first CPU to wake up, schedule a drain for the next
    // drain interval.
    bool post_drain_task = thread_sync->cpus_to_drain.none();
    thread_sync->cpus_to_drain[cpu] = true;
    if (!post_drain_task)
      return;
  }  // lock(thread_sync_.mutex)

  base::WeakPtr<FtraceController> weak_ctl = thread_sync->trace_controller_weak;
  base::TaskRunner* task_runner = thread_sync->task_runner;

  // The nested PostTask is used because the FtraceController (and hence
  // GetDrainPeriodMs()) can be called only on the main thread.
  task_runner->PostTask([weak_ctl, task_runner, generation] {

    if (!weak_ctl)
      return;
    uint32_t drain_period_ms = weak_ctl->GetDrainPeriodMs();

    task_runner->PostDelayedTask(
        [weak_ctl, generation] {
          if (weak_ctl)
            weak_ctl->DrainCPUs(generation);
        },
        drain_period_ms - (weak_ctl->NowMs() % drain_period_ms));

  });
}

// static
void FtraceController::OnCpuReaderFlush(size_t cpu,
                                        int generation,
                                        FtraceThreadSync* thread_sync) {
  // In the case of a flush, we want to drain the data as quickly as possible to
  // minimize the flush latency, at the cost of more tasks / wakeups (eventually
  // one task per cpu). Flushes are not supposed to happen too frequently.
  {
    std::lock_guard<std::mutex> lock(thread_sync->mutex);
    thread_sync->cpus_to_drain[cpu] = true;
    thread_sync->flush_acks[cpu] = true;
  }  // lock(thread_sync_.mutex)

  base::WeakPtr<FtraceController> weak_ctl = thread_sync->trace_controller_weak;
  thread_sync->task_runner->PostTask([weak_ctl, generation] {
    if (weak_ctl)
      weak_ctl->DrainCPUs(generation);
  });
}

void FtraceController::DrainCPUs(int generation) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_METATRACE("DrainCPUs()", kMainThread);

  if (generation != generation_)
    return;

  const size_t num_cpus = ftrace_procfs_->NumberOfCpus();
  PERFETTO_DCHECK(cpu_readers_.size() == num_cpus);
  FlushRequestID ack_flush_request_id = 0;
  std::bitset<base::kMaxCpus> cpus_to_drain;
  {
    std::lock_guard<std::mutex> lock(thread_sync_.mutex);
    std::swap(cpus_to_drain, thread_sync_.cpus_to_drain);

    // Check also if a flush is pending and if all cpus have acked. If that's
    // the case, ack the overall Flush() request at the end of this function.
    if (cur_flush_request_id_ && thread_sync_.flush_acks.count() >= num_cpus) {
      thread_sync_.flush_acks.reset();
      ack_flush_request_id = cur_flush_request_id_;
      cur_flush_request_id_ = 0;
    }
  }

  for (size_t cpu = 0; cpu < num_cpus; cpu++) {
    if (!cpus_to_drain[cpu])
      continue;
    // This method reads the pipe and converts the raw ftrace data into
    // protobufs using the |data_source|'s TraceWriter.
    cpu_readers_[cpu]->Drain(started_data_sources_);
    OnDrainCpuForTesting(cpu);
  }

  // If we filled up any SHM pages while draining the data, we will have posted
  // a task to notify traced about this. Only unblock the readers after this
  // notification is sent to make it less likely that they steal CPU time away
  // from traced. Also, don't unblock the readers until all of them have replied
  // to the flush.
  if (!cur_flush_request_id_) {
    base::WeakPtr<FtraceController> weak_this = weak_factory_.GetWeakPtr();
    task_runner_->PostTask([weak_this] {
      if (weak_this)
        weak_this->UnblockReaders();
    });
  }

  observer_->OnFtraceDataWrittenIntoDataSourceBuffers();

  if (ack_flush_request_id) {
    // Flush completed, all CpuReader(s) acked.

    IssueThreadSyncCmd(FtraceThreadSync::kRun);  // Switch back to reading mode.

    // This will call FtraceDataSource::OnFtraceFlushComplete(), which in turn
    // will flush the userspace buffers and ack the flush to the ProbesProducer
    // which in turn will ack the flush to the tracing service.
    NotifyFlushCompleteToStartedDataSources(ack_flush_request_id);
  }
}

void FtraceController::UnblockReaders() {
  PERFETTO_METATRACE("UnblockReaders()", kMainThread);

  // If a flush or a quit is pending, do nothing.
  std::unique_lock<std::mutex> lock(thread_sync_.mutex);
  if (thread_sync_.cmd != FtraceThreadSync::kRun)
    return;

  // Unblock all waiting readers to start moving more data into their
  // respective staging pipes.
  IssueThreadSyncCmd(FtraceThreadSync::kRun, std::move(lock));
}

void FtraceController::StartIfNeeded() {
  if (started_data_sources_.size() > 1)
    return;
  PERFETTO_DCHECK(!started_data_sources_.empty());
  PERFETTO_DCHECK(cpu_readers_.empty());
  base::WeakPtr<FtraceController> weak_this = weak_factory_.GetWeakPtr();

  {
    std::lock_guard<std::mutex> lock(thread_sync_.mutex);
    thread_sync_.cpus_to_drain.reset();
    thread_sync_.cmd = FtraceThreadSync::kRun;
    thread_sync_.cmd_id++;
  }

  generation_++;
  cpu_readers_.clear();
  cpu_readers_.reserve(ftrace_procfs_->NumberOfCpus());
  for (size_t cpu = 0; cpu < ftrace_procfs_->NumberOfCpus(); cpu++) {
    cpu_readers_.emplace_back(
        new CpuReader(table_.get(), &thread_sync_, cpu, generation_,
                      ftrace_procfs_->OpenPipeForCpu(cpu)));
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

void FtraceController::Flush(FlushRequestID flush_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  if (flush_id == cur_flush_request_id_)
    return;  // Already dealing with this flush request.

  cur_flush_request_id_ = flush_id;
  {
    std::unique_lock<std::mutex> lock(thread_sync_.mutex);
    thread_sync_.flush_acks.reset();
    IssueThreadSyncCmd(FtraceThreadSync::kFlush, std::move(lock));
  }

  base::WeakPtr<FtraceController> weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this, flush_id] {
        if (weak_this)
          weak_this->OnFlushTimeout(flush_id);
      },
      kFlushTimeoutMs);
}

void FtraceController::OnFlushTimeout(FlushRequestID flush_request_id) {
  if (flush_request_id != cur_flush_request_id_)
    return;

  uint64_t acks = 0;  // For debugging purposes only.
  {
    // Unlock the cpu readers and move on.
    std::unique_lock<std::mutex> lock(thread_sync_.mutex);
    acks = thread_sync_.flush_acks.to_ulong();
    thread_sync_.flush_acks.reset();
    if (thread_sync_.cmd == FtraceThreadSync::kFlush)
      IssueThreadSyncCmd(FtraceThreadSync::kRun, std::move(lock));
  }

  PERFETTO_ELOG("Ftrace flush(%" PRIu64 ") timed out. Acked cpus: 0x%" PRIx64,
                flush_request_id, acks);
  cur_flush_request_id_ = 0;
  NotifyFlushCompleteToStartedDataSources(flush_request_id);
}

void FtraceController::StopIfNeeded() {
  if (!started_data_sources_.empty())
    return;

  // We are not implicitly flushing on Stop. The tracing service is supposed to
  // ask for an explicit flush before stopping, unless it needs to perform a
  // non-graceful stop.

  IssueThreadSyncCmd(FtraceThreadSync::kQuit);

  // Destroying the CpuReader(s) will join on their worker threads.
  cpu_readers_.clear();
  generation_++;
}

bool FtraceController::AddDataSource(FtraceDataSource* data_source) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!ValidConfig(data_source->config()))
    return false;

  auto config_id = ftrace_config_muxer_->SetupConfig(data_source->config());
  if (!config_id)
    return false;

  const EventFilter* filter = ftrace_config_muxer_->GetEventFilter(config_id);
  auto it_and_inserted = data_sources_.insert(data_source);
  PERFETTO_DCHECK(it_and_inserted.second);
  data_source->Initialize(config_id, filter);
  return true;
}

bool FtraceController::StartDataSource(FtraceDataSource* data_source) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DCHECK(data_sources_.count(data_source) > 0);

  FtraceConfigId config_id = data_source->config_id();
  PERFETTO_CHECK(config_id);

  if (!ftrace_config_muxer_->ActivateConfig(config_id))
    return false;

  started_data_sources_.insert(data_source);
  StartIfNeeded();
  return true;
}

void FtraceController::RemoveDataSource(FtraceDataSource* data_source) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  started_data_sources_.erase(data_source);
  size_t removed = data_sources_.erase(data_source);
  if (!removed)
    return;  // Can happen if AddDataSource failed (e.g. too many sessions).
  ftrace_config_muxer_->RemoveConfig(data_source->config_id());
  StopIfNeeded();
}

void FtraceController::DumpFtraceStats(FtraceStats* stats) {
  DumpAllCpuStats(ftrace_procfs_.get(), stats);
}

void FtraceController::IssueThreadSyncCmd(
    FtraceThreadSync::Cmd cmd,
    std::unique_lock<std::mutex> pass_lock_from_caller) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  {
    std::unique_lock<std::mutex> lock(std::move(pass_lock_from_caller));
    if (!lock.owns_lock())
      lock = std::unique_lock<std::mutex>(thread_sync_.mutex);

    if (thread_sync_.cmd == FtraceThreadSync::kQuit &&
        cmd != FtraceThreadSync::kQuit) {
      // If in kQuit state, we should never issue any other commands.
      return;
    }

    thread_sync_.cmd = cmd;
    thread_sync_.cmd_id++;
  }

  // Send a SIGPIPE to all worker threads to wake them up if they are sitting in
  // a blocking splice(). If they are not and instead they are sitting in the
  // cond-variable.wait(), this, together with the one below, will have at best
  // the same effect of a spurious wakeup, depending on the implementation of
  // the condition variable.
  for (const auto& cpu_reader : cpu_readers_)
    cpu_reader->InterruptWorkerThreadWithSignal();

  thread_sync_.cond.notify_all();
}

void FtraceController::NotifyFlushCompleteToStartedDataSources(
    FlushRequestID flush_request_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  for (FtraceDataSource* data_source : started_data_sources_)
    data_source->OnFtraceFlushComplete(flush_request_id);
}

FtraceController::Observer::~Observer() = default;

}  // namespace perfetto
