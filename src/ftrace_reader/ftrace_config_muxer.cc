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

#include "ftrace_config_muxer.h"

#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>

#include "atrace_wrapper.h"
#include "perfetto/base/utils.h"
#include "proto_translation_table.h"

namespace perfetto {
namespace {

// trace_clocks in preference order.
const char* kClocks[] = {"boot", "global", "local"};

const int kDefaultPerCpuBufferSizeKb = 512;   // 512kb
const int kMaxPerCpuBufferSizeKb = 2 * 1024;  // 2mb

std::vector<std::string> difference(const std::set<std::string>& a,
                                    const std::set<std::string>& b) {
  std::vector<std::string> result;
  result.reserve(std::max(b.size(), a.size()));
  std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                      std::inserter(result, result.begin()));
  return result;
}

}  // namespace

std::set<std::string> GetFtraceEvents(const FtraceConfig& request) {
  std::set<std::string> events;
  events.insert(request.ftrace_events().begin(), request.ftrace_events().end());
  if (RequiresAtrace(request)) {
    events.insert("print");
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
  if (requested_buffer_size_kb > kMaxPerCpuBufferSizeKb)
    requested_buffer_size_kb = kDefaultPerCpuBufferSizeKb;

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
    if (RequiresAtrace(request))
      EnableAtrace(request);
    SetupClock(request);
    SetupBufferSize(request);
  } else {
    // Did someone turn ftrace off behind our back? If so give up.
    if (!is_ftrace_enabled)
      return 0;
  }

  std::set<std::string> events = GetFtraceEvents(request);

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

void FtraceConfigMuxer::EnableAtrace(const FtraceConfig& request) {
  PERFETTO_DCHECK(!current_state_.atrace_on);

  PERFETTO_DLOG("Start atrace...");

  std::vector<std::string> args;
  args.push_back("atrace");  // argv0 for exec()
  args.push_back("--async_start");
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

  if (RunAtrace({"atrace", "--async_stop"}))
    current_state_.atrace_on = false;

  PERFETTO_DLOG("...done");
}

}  // namespace perfetto
