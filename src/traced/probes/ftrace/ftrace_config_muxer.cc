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

#include "src/traced/probes/ftrace/ftrace_config_muxer.h"

#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>

#include "perfetto/base/utils.h"
#include "src/traced/probes/ftrace/atrace_wrapper.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"

namespace perfetto {
namespace {

// trace_clocks in preference order.
constexpr const char* kClocks[] = {"boot", "global", "local"};

constexpr int kDefaultPerCpuBufferSizeKb = 512;    // 512kb
constexpr int kMaxPerCpuBufferSizeKb = 64 * 1024;  // 64mb

std::vector<std::string> difference(const std::set<std::string>& a,
                                    const std::set<std::string>& b) {
  std::vector<std::string> result;
  result.reserve(std::max(b.size(), a.size()));
  std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                      std::inserter(result, result.begin()));
  return result;
}

void AddEventGroup(const ProtoTranslationTable* table,
                   const std::string& group,
                   std::set<std::string>* to) {
  const std::vector<const Event*>* events = table->GetEventsByGroup(group);
  if (!events)
    return;
  for (const Event* event : *events)
    to->insert(event->name);
}

}  // namespace

std::set<std::string> GetFtraceEvents(const FtraceConfig& request,
                                      const ProtoTranslationTable* table) {
  std::set<std::string> events;
  events.insert(request.ftrace_events().begin(), request.ftrace_events().end());
  if (RequiresAtrace(request)) {
    events.insert("print");

    // Ideally we should keep this code in sync with:
    // platform/frameworks/native/cmds/atrace/atrace.cpp
    // It's not a disaster if they go out of sync, we can always add the ftrace
    // categories manually server side but this is user friendly and reduces the
    // size of the configs.
    for (const std::string& category : request.atrace_categories()) {
      if (category == "gfx") {
        AddEventGroup(table, "mdss", &events);
        AddEventGroup(table, "sde", &events);
        continue;
      }

      if (category == "sched") {
        events.insert("sched_switch");
        events.insert("sched_wakeup");
        events.insert("sched_waking");
        events.insert("sched_blocked_reason");
        events.insert("sched_cpu_hotplug");
        AddEventGroup(table, "cgroup", &events);
        continue;
      }

      if (category == "irq") {
        AddEventGroup(table, "irq", &events);
        AddEventGroup(table, "ipi", &events);
        continue;
      }

      if (category == "irqoff") {
        events.insert("irq_enable");
        events.insert("irq_disable");
        continue;
      }

      if (category == "preemptoff") {
        events.insert("preempt_enable");
        events.insert("preempt_disable");
        continue;
      }

      if (category == "i2c") {
        AddEventGroup(table, "i2c", &events);
        continue;
      }

      if (category == "freq") {
        events.insert("cpu_frequency");
        events.insert("clock_set_rate");
        events.insert("clock_disable");
        events.insert("clock_enable");
        events.insert("clk_set_rate");
        events.insert("clk_disable");
        events.insert("clk_enable");
        events.insert("cpu_frequency_limits");
        continue;
      }

      if (category == "membus") {
        AddEventGroup(table, "memory_bus", &events);
        continue;
      }

      if (category == "idle") {
        events.insert("cpu_idle");
        continue;
      }

      if (category == "disk") {
        events.insert("f2fs_sync_file_enter");
        events.insert("f2fs_sync_file_exit");
        events.insert("f2fs_write_begin");
        events.insert("f2fs_write_end");
        events.insert("ext4_da_write_begin");
        events.insert("ext4_da_write_end");
        events.insert("ext4_sync_file_enter");
        events.insert("ext4_sync_file_exit");
        events.insert("block_rq_issue");
        events.insert("block_rq_complete");
        continue;
      }

      if (category == "mmc") {
        AddEventGroup(table, "mmc", &events);
        continue;
      }

      if (category == "load") {
        AddEventGroup(table, "cpufreq_interactive", &events);
        continue;
      }

      if (category == "sync") {
        AddEventGroup(table, "sync", &events);
        continue;
      }

      if (category == "workq") {
        AddEventGroup(table, "workqueue", &events);
        continue;
      }

      if (category == "memreclaim") {
        events.insert("mm_vmscan_direct_reclaim_begin");
        events.insert("mm_vmscan_direct_reclaim_end");
        events.insert("mm_vmscan_kswapd_wake");
        events.insert("mm_vmscan_kswapd_sleep");
        AddEventGroup(table, "lowmemorykiller", &events);
        continue;
      }

      if (category == "regulators") {
        AddEventGroup(table, "regulator", &events);
        continue;
      }

      if (category == "binder_driver") {
        events.insert("binder_transaction");
        events.insert("binder_transaction_received");
        events.insert("binder_set_priority");
        continue;
      }

      if (category == "binder_lock") {
        events.insert("binder_lock");
        events.insert("binder_locked");
        events.insert("binder_unlock");
        continue;
      }

      if (category == "pagecache") {
        AddEventGroup(table, "pagecache", &events);
        continue;
      }
    }
  }
  return events;
}

// Post-conditions:
// 1. result >= 1 (should have at least one page per CPU)
// 2. result * 4 < kMaxTotalBufferSizeKb
// 3. If input is 0 output is a good default number.
size_t ComputeCpuBufferSizeInPages(size_t requested_buffer_size_kb) {
  if (requested_buffer_size_kb == 0)
    requested_buffer_size_kb = kDefaultPerCpuBufferSizeKb;
  if (requested_buffer_size_kb > kMaxPerCpuBufferSizeKb) {
    PERFETTO_ELOG(
        "The requested ftrace buf size (%zu KB) is too big, capping to %d KB",
        requested_buffer_size_kb, kMaxPerCpuBufferSizeKb);
    requested_buffer_size_kb = kMaxPerCpuBufferSizeKb;
  }

  size_t pages = requested_buffer_size_kb / (base::kPageSize / 1024);
  if (pages == 0)
    return 1;

  return pages;
}

FtraceConfigMuxer::FtraceConfigMuxer(FtraceProcfs* ftrace,
                                     const ProtoTranslationTable* table)
    : ftrace_(ftrace), table_(table), current_state_(), configs_() {}
FtraceConfigMuxer::~FtraceConfigMuxer() = default;

FtraceConfigId FtraceConfigMuxer::RequestConfig(const FtraceConfig& request) {
  FtraceConfig actual;

  bool is_ftrace_enabled = ftrace_->IsTracingEnabled();
  if (configs_.empty()) {
    PERFETTO_DCHECK(!current_state_.tracing_on);

    // If someone outside of perfetto is using ftrace give up now.
    if (is_ftrace_enabled)
      return 0;

    // If we're about to turn tracing on use this opportunity do some setup:
    SetupClock(request);
    SetupBufferSize(request);
  } else {
    // Did someone turn ftrace off behind our back? If so give up.
    if (!is_ftrace_enabled)
      return 0;
  }

  std::set<std::string> events = GetFtraceEvents(request, table_);

  if (RequiresAtrace(request))
    UpdateAtrace(request);

  for (auto& name : events) {
    const Event* event = table_->GetEventByName(name);
    if (!event) {
      PERFETTO_DLOG("Can't enable %s, event not known", name.c_str());
      continue;
    }
    if (current_state_.ftrace_events.count(name) ||
        std::string("ftrace") == event->group) {
      *actual.add_ftrace_events() = name;
      continue;
    }
    if (ftrace_->EnableEvent(event->group, event->name)) {
      current_state_.ftrace_events.insert(name);
      *actual.add_ftrace_events() = name;
    } else {
      PERFETTO_DPLOG("Failed to enable %s.", name.c_str());
    }
  }

  if (configs_.empty()) {
    PERFETTO_DCHECK(!current_state_.tracing_on);
    ftrace_->EnableTracing();
    current_state_.tracing_on = true;
  }

  FtraceConfigId id = ++last_id_;
  configs_.emplace(id, std::move(actual));
  return id;
}

bool FtraceConfigMuxer::RemoveConfig(FtraceConfigId id) {
  if (!id || !configs_.erase(id))
    return false;

  std::set<std::string> expected_ftrace_events;
  for (const auto& id_config : configs_) {
    const FtraceConfig& config = id_config.second;
    expected_ftrace_events.insert(config.ftrace_events().begin(),
                                  config.ftrace_events().end());
  }

  std::vector<std::string> events_to_disable =
      difference(current_state_.ftrace_events, expected_ftrace_events);

  for (auto& name : events_to_disable) {
    const Event* event = table_->GetEventByName(name);
    if (!event)
      continue;
    if (ftrace_->DisableEvent(event->group, event->name))
      current_state_.ftrace_events.erase(name);
  }

  if (configs_.empty()) {
    PERFETTO_DCHECK(current_state_.tracing_on);
    ftrace_->DisableTracing();
    ftrace_->SetCpuBufferSizeInPages(0);
    ftrace_->DisableAllEvents();
    ftrace_->ClearTrace();
    current_state_.tracing_on = false;
    if (current_state_.atrace_on)
      DisableAtrace();
  }

  return true;
}

const FtraceConfig* FtraceConfigMuxer::GetConfig(FtraceConfigId id) {
  if (!configs_.count(id))
    return nullptr;
  return &configs_.at(id);
}

void FtraceConfigMuxer::SetupClock(const FtraceConfig&) {
  std::string current_clock = ftrace_->GetClock();
  std::set<std::string> clocks = ftrace_->AvailableClocks();

  for (size_t i = 0; i < base::ArraySize(kClocks); i++) {
    std::string clock = std::string(kClocks[i]);
    if (!clocks.count(clock))
      continue;
    if (current_clock == clock)
      break;
    ftrace_->SetClock(clock);
    break;
  }
}

void FtraceConfigMuxer::SetupBufferSize(const FtraceConfig& request) {
  size_t pages = ComputeCpuBufferSizeInPages(request.buffer_size_kb());
  ftrace_->SetCpuBufferSizeInPages(pages);
  current_state_.cpu_buffer_size_pages = pages;
}

void FtraceConfigMuxer::UpdateAtrace(const FtraceConfig& request) {
  PERFETTO_DLOG("Update atrace config...");

  std::vector<std::string> args;
  args.push_back("atrace");  // argv0 for exec()
  args.push_back("--async_start");
  args.push_back("--only_userspace");
  for (const auto& category : request.atrace_categories())
    args.push_back(category);
  if (!request.atrace_apps().empty()) {
    args.push_back("-a");
    for (const auto& app : request.atrace_apps())
      args.push_back(app);
  }

  if (RunAtrace(args))
    current_state_.atrace_on = true;

  PERFETTO_DLOG("...done");
}

void FtraceConfigMuxer::DisableAtrace() {
  PERFETTO_DCHECK(current_state_.atrace_on);

  PERFETTO_DLOG("Stop atrace...");

  if (RunAtrace({"atrace", "--async_stop", "--only_userspace"}))
    current_state_.atrace_on = false;

  PERFETTO_DLOG("...done");
}

}  // namespace perfetto
