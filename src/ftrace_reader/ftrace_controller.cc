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

#include "perfetto/ftrace_reader/ftrace_controller.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <array>
#include <string>

#include "cpu_reader.h"
#include "event_info.h"
#include "ftrace_procfs.h"
#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"
#include "proto_translation_table.h"

#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto {
namespace {

#if BUILDFLAG(OS_ANDROID)
const char* kTracingPaths[] = {
    "/sys/kernel/tracing/", "/sys/kernel/debug/tracing/", nullptr,
};
#else
const char* kTracingPaths[] = {
    "/sys/kernel/debug/tracing/", nullptr,
};
#endif

const int kDefaultDrainPeriodMs = 100;
const int kMinDrainPeriodMs = 1;
const int kMaxDrainPeriodMs = 1000 * 60;

const int kDefaultTotalBufferSizeKb = 1024 * 4;  // 4mb
const int kMaxTotalBufferSizeKb = 1024 * 8;      // 8mb

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

// Post-conditions:
// 1. result >= 1 (should have at least one page per CPU)
// 2. result * 4 < kMaxTotalBufferSizeKb
// 3. If input is 0 output is a good default number.
size_t ComputeCpuBufferSizeInPages(uint32_t requested_buffer_size_kb) {
  if (requested_buffer_size_kb == 0)
    requested_buffer_size_kb = kDefaultTotalBufferSizeKb;
  if (requested_buffer_size_kb > kMaxTotalBufferSizeKb)
    requested_buffer_size_kb = kDefaultTotalBufferSizeKb;

  size_t pages = requested_buffer_size_kb / (base::kPageSize / 1024);
  if (pages == 0)
    return 1;

  return pages;
}

bool RunAtrace(std::vector<std::string> args) {
  int status = 1;

  std::vector<char*> argv;
  // args, and then a null.
  argv.reserve(1 + args.size());
  for (const auto& arg : args)
    argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  pid_t pid = fork();
  PERFETTO_CHECK(pid >= 0);
  if (pid == 0) {
    execv("/system/bin/atrace", &argv[0]);
    // Reached only if execv fails.
    _exit(1);
  }
  waitpid(pid, &status, 0);
  return status == 0;
}

}  // namespace

// static
// TODO(taylori): Add a test for tracing paths in integration tests.
std::unique_ptr<FtraceController> FtraceController::Create(
    base::TaskRunner* runner) {
  size_t index = 0;
  std::unique_ptr<FtraceProcfs> ftrace_procfs = nullptr;
  while (!ftrace_procfs && kTracingPaths[index]) {
    ftrace_procfs = FtraceProcfs::Create(kTracingPaths[index++]);
  }

  if (!ftrace_procfs) {
    return nullptr;
  }

  auto table = ProtoTranslationTable::Create(
      ftrace_procfs.get(), GetStaticEventInfo(), GetStaticCommonFieldsInfo());
  return std::unique_ptr<FtraceController>(
      new FtraceController(std::move(ftrace_procfs), runner, std::move(table)));
}

FtraceController::FtraceController(std::unique_ptr<FtraceProcfs> ftrace_procfs,
                                   base::TaskRunner* task_runner,
                                   std::unique_ptr<ProtoTranslationTable> table)
    : ftrace_procfs_(std::move(ftrace_procfs)),
      task_runner_(task_runner),
      enabled_count_(table->largest_id() + 1),
      table_(std::move(table)),
      weak_factory_(this) {}

FtraceController::~FtraceController() {
  for (size_t id = 1; id <= table_->largest_id(); id++) {
    if (enabled_count_[id]) {
      const Event* event = table_->GetEventById(id);
      ftrace_procfs_->DisableEvent(event->group, event->name);
    }
  }
  if (listening_for_raw_trace_data_) {
    sinks_.clear();
    StopIfNeeded();
  }
}

// static
void FtraceController::PeriodicDrainCPU(
    base::WeakPtr<FtraceController> weak_this,
    size_t generation,
    int cpu) {
  // The controller might be gone.
  if (!weak_this)
    return;
  // We might have stopped caring about events.
  if (!weak_this->listening_for_raw_trace_data_)
    return;
  // We might have stopped tracing then quickly re-enabled it, in this case
  // we don't want to end up with two periodic tasks for each CPU:
  if (weak_this->generation_ != generation)
    return;

  bool has_more = weak_this->OnRawFtraceDataAvailable(cpu);
  weak_this->task_runner_->PostDelayedTask(
      std::bind(&FtraceController::PeriodicDrainCPU, weak_this, generation,
                cpu),
      has_more ? 0 : weak_this->GetDrainPeriodMs());
}

void FtraceController::StartIfNeeded() {
  if (sinks_.size() > 1)
    return;
  PERFETTO_CHECK(sinks_.size() != 0);
  PERFETTO_CHECK(!listening_for_raw_trace_data_);
  listening_for_raw_trace_data_ = true;
  ftrace_procfs_->SetCpuBufferSizeInPages(GetCpuBufferSizeInPages());
  ftrace_procfs_->EnableTracing();
  generation_++;
  for (size_t cpu = 0; cpu < ftrace_procfs_->NumberOfCpus(); cpu++) {
    base::WeakPtr<FtraceController> weak_this = weak_factory_.GetWeakPtr();
    task_runner_->PostDelayedTask(std::bind(&FtraceController::PeriodicDrainCPU,
                                            weak_this, generation_, cpu),
                                  GetDrainPeriodMs());
  }
}

uint32_t FtraceController::GetDrainPeriodMs() {
  if (sinks_.size() == 0)
    return kDefaultDrainPeriodMs;
  uint32_t min_drain_period_ms = kMaxDrainPeriodMs + 1;
  for (const FtraceSink* sink : sinks_) {
    if (sink->config().drain_period_ms() < min_drain_period_ms)
      min_drain_period_ms = sink->config().drain_period_ms();
  }
  return ClampDrainPeriodMs(min_drain_period_ms);
}

uint32_t FtraceController::GetCpuBufferSizeInPages() {
  uint32_t max_buffer_size_kb = 0;
  for (const FtraceSink* sink : sinks_) {
    if (sink->config().buffer_size_kb() > max_buffer_size_kb)
      max_buffer_size_kb = sink->config().buffer_size_kb();
  }
  return ComputeCpuBufferSizeInPages(max_buffer_size_kb);
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
  if (sinks_.size() != 0)
    return;
  PERFETTO_CHECK(listening_for_raw_trace_data_);
  listening_for_raw_trace_data_ = false;
  readers_.clear();
  ftrace_procfs_->DisableTracing();
}

bool FtraceController::OnRawFtraceDataAvailable(size_t cpu) {
  CpuReader* reader = GetCpuReader(cpu);
  using BundleHandle =
      protozero::ProtoZeroMessageHandle<protos::pbzero::FtraceEventBundle>;
  std::array<const EventFilter*, kMaxSinks> filters{};
  std::array<BundleHandle, kMaxSinks> bundles{};
  size_t sink_count = sinks_.size();
  size_t i = 0;
  for (FtraceSink* sink : sinks_) {
    filters[i] = sink->get_event_filter();
    bundles[i++] = sink->GetBundleForCpu(cpu);
  }
  bool res = reader->Drain(filters, bundles);
  i = 0;
  for (FtraceSink* sink : sinks_)
    sink->OnBundleComplete(cpu, std::move(bundles[i++]));
  PERFETTO_DCHECK(sinks_.size() == sink_count);
  return res;
}

CpuReader* FtraceController::GetCpuReader(size_t cpu) {
  PERFETTO_CHECK(cpu < ftrace_procfs_->NumberOfCpus());
  if (!readers_.count(cpu)) {
    readers_.emplace(
        cpu, std::unique_ptr<CpuReader>(new CpuReader(
                 table_.get(), cpu, ftrace_procfs_->OpenPipeForCpu(cpu))));
  }
  return readers_.at(cpu).get();
}

std::unique_ptr<FtraceSink> FtraceController::CreateSink(
    FtraceConfig config,
    FtraceSink::Delegate* delegate) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (sinks_.size() >= kMaxSinks)
    return nullptr;
  if (!ValidConfig(config))
    return nullptr;
  auto controller_weak = weak_factory_.GetWeakPtr();
  auto filter = std::unique_ptr<EventFilter>(
      new EventFilter(*table_.get(), FtraceEventsAsSet(config)));
  for (const std::string& event : config.event_names())
    PERFETTO_LOG("%s", event.c_str());
  auto sink = std::unique_ptr<FtraceSink>(new FtraceSink(
      std::move(controller_weak), config, std::move(filter), delegate));
  Register(sink.get());
  return sink;
}

void FtraceController::Register(FtraceSink* sink) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto it_and_inserted = sinks_.insert(sink);
  PERFETTO_DCHECK(it_and_inserted.second);
  if (RequiresAtrace(sink->config()))
    StartAtrace(sink->config());

  StartIfNeeded();
  for (const std::string& name : sink->enabled_events())
    RegisterForEvent(name);
}

void FtraceController::RegisterForEvent(const std::string& name) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  const Event* event = table_->GetEventByName(name);
  if (!event) {
    PERFETTO_DLOG("Can't enable %s, event not known", name.c_str());
    return;
  }
  size_t& count = enabled_count_.at(event->ftrace_event_id);
  if (count == 0)
    ftrace_procfs_->EnableEvent(event->group, event->name);
  count += 1;
}

void FtraceController::UnregisterForEvent(const std::string& name) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  const Event* event = table_->GetEventByName(name);
  if (!event)
    return;
  size_t& count = enabled_count_.at(event->ftrace_event_id);
  PERFETTO_CHECK(count > 0);
  if (--count == 0)
    ftrace_procfs_->DisableEvent(event->group, event->name);
}

void FtraceController::Unregister(FtraceSink* sink) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  size_t removed = sinks_.erase(sink);
  PERFETTO_DCHECK(removed == 1);

  for (const std::string& name : sink->enabled_events())
    UnregisterForEvent(name);
  if (RequiresAtrace(sink->config()))
    StopAtrace();
  StopIfNeeded();
}

void FtraceController::StartAtrace(const FtraceConfig& config) {
  PERFETTO_CHECK(atrace_running_ == false);
  atrace_running_ = true;
  PERFETTO_DLOG("Start atrace...");
  std::vector<std::string> args;
  args.push_back("atrace");  // argv0 for exec()
  args.push_back("--async_start");
  for (const auto& category : config.atrace_categories())
    args.push_back(category);
  if (!config.atrace_apps().empty()) {
    args.push_back("-a");
    for (const auto& app : config.atrace_apps())
      args.push_back(app);
  }

  PERFETTO_CHECK(RunAtrace(std::move(args)));
  PERFETTO_DLOG("...done");
}

void FtraceController::StopAtrace() {
  PERFETTO_CHECK(atrace_running_ == true);
  atrace_running_ = false;
  PERFETTO_DLOG("Stop atrace...");
  PERFETTO_CHECK(
      RunAtrace(std::vector<std::string>({"atrace", "--async_stop"})));
  PERFETTO_DLOG("...done");
}

FtraceSink::FtraceSink(base::WeakPtr<FtraceController> controller_weak,
                       FtraceConfig config,
                       std::unique_ptr<EventFilter> filter,
                       Delegate* delegate)
    : controller_weak_(std::move(controller_weak)),
      config_(config),
      filter_(std::move(filter)),
      delegate_(delegate){};

FtraceSink::~FtraceSink() {
  if (controller_weak_)
    controller_weak_->Unregister(this);
};

const std::set<std::string>& FtraceSink::enabled_events() {
  return filter_->enabled_names();
}

}  // namespace perfetto
