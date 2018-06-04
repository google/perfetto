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
#include "src/traced/probes/ftrace/cpu_reader.h"
#include "src/traced/probes/ftrace/cpu_stats_parser.h"
#include "src/traced/probes/ftrace/event_info.h"
#include "src/traced/probes/ftrace/ftrace_config_muxer.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"

#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "perfetto/trace/ftrace/ftrace_stats.pbzero.h"

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
    base::TaskRunner* runner) {
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
  return std::unique_ptr<FtraceController>(new FtraceController(
      std::move(ftrace_procfs), std::move(table), std::move(model), runner));
}

FtraceController::FtraceController(std::unique_ptr<FtraceProcfs> ftrace_procfs,
                                   std::unique_ptr<ProtoTranslationTable> table,
                                   std::unique_ptr<FtraceConfigMuxer> model,
                                   base::TaskRunner* task_runner)
    : ftrace_procfs_(std::move(ftrace_procfs)),
      table_(std::move(table)),
      ftrace_config_muxer_(std::move(model)),
      task_runner_(task_runner),
      weak_factory_(this) {}

FtraceController::~FtraceController() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  for (const auto* sink : sinks_)
    ftrace_config_muxer_->RemoveConfig(sink->id_);
  sinks_.clear();
  StopIfNeeded();
}

uint64_t FtraceController::NowMs() const {
  return static_cast<uint64_t>(base::GetWallTimeMs().count());
}

// static
void FtraceController::DrainCPUs(base::WeakPtr<FtraceController> weak_this,
                                 size_t generation) {
  // The controller might be gone.
  if (!weak_this)
    return;
  // We might have stopped tracing then quickly re-enabled it, in this case
  // we don't want to end up with two periodic tasks for each CPU:
  if (weak_this->generation_ != generation)
    return;

  PERFETTO_DCHECK_THREAD(weak_this->thread_checker_);
  std::bitset<kMaxCpus> cpus_to_drain;
  {
    std::unique_lock<std::mutex> lock(weak_this->lock_);
    // We might have stopped caring about events.
    if (!weak_this->listening_for_raw_trace_data_)
      return;
    std::swap(cpus_to_drain, weak_this->cpus_to_drain_);
  }

  for (size_t cpu = 0; cpu < weak_this->ftrace_procfs_->NumberOfCpus(); cpu++) {
    if (!cpus_to_drain[cpu])
      continue;
    weak_this->OnRawFtraceDataAvailable(cpu);
  }

  // If we filled up any SHM pages while draining the data, we will have posted
  // a task to notify traced about this. Only unblock the readers after this
  // notification is sent to make it less likely that they steal CPU time away
  // from traced.
  weak_this->task_runner_->PostTask(
      std::bind(&FtraceController::UnblockReaders, weak_this));
}

// static
void FtraceController::UnblockReaders(
    const base::WeakPtr<FtraceController>& weak_this) {
  if (!weak_this)
    return;
  // Unblock all waiting readers to start moving more data into their
  // respective staging pipes.
  weak_this->data_drained_.notify_all();
}

void FtraceController::StartIfNeeded() {
  if (sinks_.size() > 1)
    return;
  PERFETTO_CHECK(!sinks_.empty());
  {
    std::unique_lock<std::mutex> lock(lock_);
    PERFETTO_CHECK(!listening_for_raw_trace_data_);
    listening_for_raw_trace_data_ = true;
  }
  generation_++;
  base::WeakPtr<FtraceController> weak_this = weak_factory_.GetWeakPtr();
  for (size_t cpu = 0; cpu < ftrace_procfs_->NumberOfCpus(); cpu++) {
    readers_.emplace(
        cpu, std::unique_ptr<CpuReader>(new CpuReader(
                 table_.get(), cpu, ftrace_procfs_->OpenPipeForCpu(cpu),
                 std::bind(&FtraceController::OnDataAvailable, this, weak_this,
                           generation_, cpu, GetDrainPeriodMs()))));
  }
}

uint32_t FtraceController::GetDrainPeriodMs() {
  if (sinks_.empty())
    return kDefaultDrainPeriodMs;
  uint32_t min_drain_period_ms = kMaxDrainPeriodMs + 1;
  for (const FtraceSink* sink : sinks_) {
    if (sink->config().drain_period_ms() < min_drain_period_ms)
      min_drain_period_ms = sink->config().drain_period_ms();
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
  if (!sinks_.empty())
    return;
  {
    // Unblock any readers that are waiting for us to drain data.
    std::unique_lock<std::mutex> lock(lock_);
    listening_for_raw_trace_data_ = false;
    cpus_to_drain_.reset();
  }
  data_drained_.notify_all();
  readers_.clear();
}

void FtraceController::OnRawFtraceDataAvailable(size_t cpu) {
  PERFETTO_CHECK(cpu < ftrace_procfs_->NumberOfCpus());
  CpuReader* reader = readers_[cpu].get();
  using BundleHandle =
      protozero::MessageHandle<protos::pbzero::FtraceEventBundle>;
  std::array<const EventFilter*, kMaxSinks> filters{};
  std::array<BundleHandle, kMaxSinks> bundles{};
  std::array<FtraceMetadata*, kMaxSinks> metadatas{};
  size_t sink_count = sinks_.size();
  size_t i = 0;
  for (FtraceSink* sink : sinks_) {
    filters[i] = sink->event_filter();
    metadatas[i] = sink->metadata_mutable();
    bundles[i++] = sink->GetBundleForCpu(cpu);
  }
  reader->Drain(filters, bundles, metadatas);
  i = 0;
  for (FtraceSink* sink : sinks_)
    sink->OnBundleComplete(cpu, std::move(bundles[i++]));
  PERFETTO_DCHECK(sinks_.size() == sink_count);
}

std::unique_ptr<FtraceSink> FtraceController::CreateSink(
    FtraceConfig config,
    FtraceSink::Delegate* delegate) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (sinks_.size() >= kMaxSinks)
    return nullptr;
  if (!ValidConfig(config))
    return nullptr;

  FtraceConfigId id = ftrace_config_muxer_->RequestConfig(config);
  if (!id)
    return nullptr;

  auto controller_weak = weak_factory_.GetWeakPtr();
  auto filter = std::unique_ptr<EventFilter>(new EventFilter(
      *table_, FtraceEventsAsSet(*ftrace_config_muxer_->GetConfig(id))));

  auto sink = std::unique_ptr<FtraceSink>(
      new FtraceSink(std::move(controller_weak), id, std::move(config),
                     std::move(filter), delegate));
  Register(sink.get());
  delegate->OnCreate(sink.get());
  return sink;
}

void FtraceController::OnDataAvailable(
    base::WeakPtr<FtraceController> weak_this,
    size_t generation,
    size_t cpu,
    uint32_t drain_period_ms) {
  // Called on the worker thread.
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

void FtraceController::Register(FtraceSink* sink) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto it_and_inserted = sinks_.insert(sink);
  PERFETTO_DCHECK(it_and_inserted.second);
  StartIfNeeded();
}

void FtraceController::Unregister(FtraceSink* sink) {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  size_t removed = sinks_.erase(sink);
  PERFETTO_DCHECK(removed == 1);

  ftrace_config_muxer_->RemoveConfig(sink->id_);

  StopIfNeeded();
}

void FtraceController::DumpFtraceStats(FtraceStats* stats) {
  DumpAllCpuStats(ftrace_procfs_.get(), stats);
}

FtraceSink::FtraceSink(base::WeakPtr<FtraceController> controller_weak,
                       FtraceConfigId id,
                       FtraceConfig config,
                       std::unique_ptr<EventFilter> filter,
                       Delegate* delegate)
    : controller_weak_(std::move(controller_weak)),
      id_(id),
      config_(std::move(config)),
      filter_(std::move(filter)),
      delegate_(delegate){};

FtraceSink::~FtraceSink() {
  if (controller_weak_)
    controller_weak_->Unregister(this);
};

const std::set<std::string>& FtraceSink::enabled_events() {
  return filter_->enabled_names();
}

void FtraceSink::DumpFtraceStats(FtraceStats* stats) {
  if (controller_weak_)
    controller_weak_->DumpFtraceStats(stats);
}

void FtraceStats::Write(protos::pbzero::FtraceStats* writer) const {
  for (const FtraceCpuStats& cpu_specific_stats : cpu_stats) {
    cpu_specific_stats.Write(writer->add_cpu_stats());
  }
}

void FtraceCpuStats::Write(protos::pbzero::FtraceCpuStats* writer) const {
  writer->set_cpu(cpu);
  writer->set_entries(entries);
  writer->set_overrun(overrun);
  writer->set_commit_overrun(commit_overrun);
  writer->set_bytes_read(bytes_read);
  writer->set_oldest_event_ts(oldest_event_ts);
  writer->set_now_ts(now_ts);
  writer->set_dropped_events(dropped_events);
  writer->set_read_events(read_events);
}

FtraceMetadata::FtraceMetadata() {
  // A lot of the time there will only be a small number of inodes.
  inode_and_device.reserve(10);
  pids.reserve(10);
}

void FtraceMetadata::AddDevice(BlockDeviceID device_id) {
  last_seen_device_id = device_id;
#if PERFETTO_DCHECK_IS_ON()
  seen_device_id = true;
#endif
}

void FtraceMetadata::AddInode(Inode inode_number) {
#if PERFETTO_DCHECK_IS_ON()
  PERFETTO_DCHECK(seen_device_id);
#endif
  static int32_t cached_pid = 0;
  if (!cached_pid)
    cached_pid = getpid();

  PERFETTO_DCHECK(last_seen_common_pid);
  PERFETTO_DCHECK(cached_pid == getpid());
  // Ignore own scanning activity.
  if (cached_pid != last_seen_common_pid) {
    inode_and_device.push_back(
        std::make_pair(inode_number, last_seen_device_id));
  }
}

void FtraceMetadata::AddCommonPid(int32_t pid) {
  last_seen_common_pid = pid;
}

void FtraceMetadata::AddPid(int32_t pid) {
  // Speculative optimization aginst repated pid's while keeping
  // faster insertion than a set.
  if (!pids.empty() && pids.back() == pid)
    return;
  pids.push_back(pid);
}

void FtraceMetadata::FinishEvent() {
  last_seen_device_id = 0;
#if PERFETTO_DCHECK_IS_ON()
  seen_device_id = false;
#endif
  last_seen_common_pid = 0;
}

void FtraceMetadata::Clear() {
  inode_and_device.clear();
  pids.clear();
  overwrite_count = 0;
  FinishEvent();
}

FtraceSink::Delegate::~Delegate() = default;

}  // namespace perfetto
