/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/profiling/smaps/smaps_data_source.h"

#include <stdio.h>

#include <algorithm>
#include <set>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/metatrace.h"
#include "perfetto/ext/base/metatrace_events.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/profiling/smaps.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "protos/perfetto/trace/profiling/smaps.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/profiling/common/proc_utils.h"

namespace perfetto {
namespace profiling {

namespace {
constexpr uint32_t kMinReadPeriodMs = 1000;                 // 1s
constexpr uint32_t kMaxReadPeriodMs = 24 * 60 * 60 * 1000;  // 24 hrs
}  // namespace

// static
std::optional<SmapsDataSource::Config> SmapsDataSource::Config::Create(
    const DataSourceConfig& ds_config) {
  protos::gen::ProcessSmapsConfig smaps_cfg_pb;
  if (!smaps_cfg_pb.ParseFromString(ds_config.process_smaps_config_raw())) {
    PERFETTO_ELOG("linux.smaps config not a valid protobuf.");
    return std::nullopt;
  }

  Config config;
  config.target_cmdlines = smaps_cfg_pb.scope().target_cmdline();
  if (config.target_cmdlines.empty()) {
    PERFETTO_ELOG(
        "linux.smaps does not specify scope.target_cmdline, rejecting data "
        "source.");
    return std::nullopt;
  }

  config.recording_config = smaps_cfg_pb.smaps_config();

  // Zero is accepted as one-shot.
  if (smaps_cfg_pb.read_period_ms() != 0) {
    config.read_period_ms = std::clamp(smaps_cfg_pb.read_period_ms(),
                                       kMinReadPeriodMs, kMaxReadPeriodMs);
    if (config.read_period_ms != smaps_cfg_pb.read_period_ms()) {
      PERFETTO_ILOG("Clamped linux.smaps read_period_ms from %" PRIu32
                    " to %" PRIu32,
                    smaps_cfg_pb.read_period_ms(), config.read_period_ms);
    }
  }

  return config;
}

SmapsDataSource::SmapsDataSource(Config config,
                                 base::TaskRunner* task_runner,
                                 std::unique_ptr<TraceWriter> trace_writer)
    : task_runner_(task_runner),
      trace_writer_(std::move(trace_writer)),
      config_(std::move(config)),
      weak_factory_(this) {}

void SmapsDataSource::SerializeSmapsForPid(pid_t pid) {
  PERFETTO_METATRACE_SCOPED(TAG_PRODUCER, LINUX_SMAPS_SERIALIZE);

  base::StackString<128> path("/proc/%d/smaps", static_cast<int>(pid));
  base::ScopedFstream smaps(fopen(path.c_str(), base::kFopenReadFlag));
  if (!smaps) {
    PERFETTO_DPLOG("linux.smaps: failed to open %s", path.c_str());
    return;
  }

  auto trace_packet = trace_writer_->NewTracePacket();
  trace_packet->set_timestamp(
      static_cast<uint64_t>(base::GetBootTimeNs().count()));

  auto* smaps_packet = trace_packet->set_smaps_packet();
  smaps_packet->set_pid(static_cast<uint32_t>(pid));
  using RT = perfetto::protos::pbzero::SmapsPacket::RecordingType;
  smaps_packet->set_recording_type(RT::RECORDING_TYPE_STANDALONE);

  ParseAndSerializeSmaps(*smaps, config_.recording_config, smaps_packet);
}

void SmapsDataSource::Start() {
  Tick();
}

// A tick scans procfs to find eligible targets, pushes their pids into a queue,
// and enqueues a self-reposting task to serialise each smaps file in a separate
// task runner task. The work is split because the task runner is shared, but
// smaps parsing can be >10ms per pid in the worst case.
void SmapsDataSource::Tick() {
  if (stopping_)
    return;

  if (pending_reads_.empty()) {
    QueueSmapsReads();
  } else {
    PERFETTO_DLOG("Skipping linux.smaps tick, overrunning.");
  }

  uint32_t period_ms = config_.read_period_ms;
  if (period_ms == 0)
    return;  // one-shot recording, nothing left to do

  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this] {
        if (weak_this)
          weak_this->Tick();
      },
      period_ms);
}

void SmapsDataSource::QueueSmapsReads() {
  PERFETTO_METATRACE_SCOPED(TAG_PRODUCER, LINUX_SMAPS_ENQUEUE);

  // Note: the set of matching processes is re-evaluated on every tick to catch
  // new or renamed processes.
  std::set<pid_t> target_pids;
  glob_aware::FindPidsForCmdlinePatterns(config_.target_cmdlines, &target_pids);
  if (target_pids.empty())
    return;

  pending_reads_.assign(target_pids.begin(), target_pids.end());

  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_this] {
    if (weak_this)
      weak_this->ReadOnePending();
  });
}

void SmapsDataSource::ReadOnePending() {
  if (pending_reads_.empty())
    return;
  pid_t pid = pending_reads_.back();
  pending_reads_.pop_back();
  SerializeSmapsForPid(pid);

  // If we're stopping, do so if queue is empty.
  if (pending_reads_.empty() && stopping_) {
    FinishStop();
    return;
  }

  // Repost continuation if not done.
  if (!pending_reads_.empty()) {
    auto weak_this = weak_factory_.GetWeakPtr();
    task_runner_->PostTask([weak_this] {
      if (weak_this)
        weak_this->ReadOnePending();
    });
  }
}

void SmapsDataSource::Flush() {
  trace_writer_->Flush();
}

void SmapsDataSource::Stop(std::function<void()> on_stopped) {
  PERFETTO_CHECK(!stopping_);
  stopping_ = true;
  on_stopped_ = std::move(on_stopped);

  // Stop immediately if nothing enqueued, otherwise let |ReadOnePending| finish
  // once the queue is empty.
  if (pending_reads_.empty()) {
    FinishStop();
  }
}

void SmapsDataSource::FinishStop() {
  PERFETTO_CHECK(stopping_ && pending_reads_.empty());
  Flush();

  // Run the cleanup function as a separate task so that it can destroy this
  // instance.
  task_runner_->PostTask(
      [on_stopped = std::move(on_stopped_)] { on_stopped(); });
}

}  // namespace profiling
}  // namespace perfetto
