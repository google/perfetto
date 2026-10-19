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

#include "src/trace_processor/importers/systrace/systrace_line_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/thread_state_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/tracks_common.h"
#include "src/trace_processor/importers/ftrace/binder_tracker.h"
#include "src/trace_processor/importers/ftrace/ftrace_sched_event_tracker.h"
#include "src/trace_processor/importers/systrace/systrace_line.h"
#include "src/trace_processor/importers/systrace/systrace_parser.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/task_state.h"

namespace perfetto::trace_processor {

SystraceLineParser::SystraceLineParser(TraceProcessorContext* ctx)
    : context_(ctx),
      rss_stat_tracker_(context_),
      sched_wakeup_name_id_(ctx->storage->InternString("sched_wakeup")),
      sched_waking_name_id_(ctx->storage->InternString("sched_waking")),
      workqueue_name_id_(ctx->storage->InternString("workqueue")),
      sched_blocked_reason_id_(
          ctx->storage->InternString("sched_blocked_reason")),
      io_wait_id_(ctx->storage->InternString("io_wait")),
      waker_utid_id_(ctx->storage->InternString("waker_utid")),
      unknown_thread_name_id_(ctx->storage->InternString("<...>")) {}

base::Status SystraceLineParser::ParseLine(const SystraceLine& line) {
  const StringId line_task_id{
      context_->storage->InternString(base::StringView(line.task))};
  auto utid = context_->process_tracker->GetOrCreateThread(line.pid);
  context_->process_tracker->UpdateThreadName(
      utid,
      // Ftrace doesn't always know the thread name (see ftrace documentation
      // for saved_cmdlines) so some lines name a process "<...>". Don't use
      // this bogus name for thread naming otherwise a real name from a previous
      // line could be overwritten.
      line_task_id == unknown_thread_name_id_ ? StringId::Null() : line_task_id,
      ThreadNamePriority::kFtrace);

  if (!line.tgid_str.empty() && line.tgid_str != "-----") {
    std::optional<uint32_t> tgid =
        base::StringViewToNumber<uint32_t>(base::StringView(line.tgid_str));
    if (tgid) {
      context_->process_tracker->UpdateThread(line.pid, tgid.value());
    }
  }

  // Fast path: tracing_mark_write, 0, and print events are the most frequent
  // events in userspace systrace/atrace traces and do not use key-value args.
  if (line.event_name == "tracing_mark_write" || line.event_name == "0" ||
      line.event_name == "print") {
    SystraceParser::GetOrCreate(context_)->ParsePrintEvent(
        line.ts, line.pid, line.args_str.c_str());
    return base::OkStatus();
  }

  // Fast path: workqueue events and binder lock events do not use key-value args.
  if (line.event_name == "workqueue_execute_start") {
    auto split = base::SplitString(line.args_str, "function ");
    if (split.size() < 2) {
      return base::ErrStatus("Could not parse workqueue_execute_start");
    }
    StringId name_id =
        context_->storage->InternString(base::StringView(split[1]));
    TrackId track = context_->track_tracker->InternThreadTrack(utid);
    context_->slice_tracker->Begin(line.ts, track, workqueue_name_id_, name_id);
    return base::OkStatus();
  }
  if (line.event_name == "workqueue_execute_end") {
    TrackId track = context_->track_tracker->InternThreadTrack(utid);
    context_->slice_tracker->End(line.ts, track, workqueue_name_id_);
    return base::OkStatus();
  }
  if (line.event_name == "binder_lock") {
    BinderTracker::GetOrCreate(context_)->Lock(line.ts, line.pid);
    return base::OkStatus();
  }
  if (line.event_name == "binder_locked") {
    BinderTracker::GetOrCreate(context_)->Locked(line.ts, line.pid);
    return base::OkStatus();
  }
  if (line.event_name == "binder_unlock") {
    BinderTracker::GetOrCreate(context_)->Unlock(line.ts, line.pid);
    return base::OkStatus();
  }

  const bool needs_args =
      line.event_name == "sched_switch" || line.event_name == "sched_waking" ||
      line.event_name == "cpu_frequency" ||
      line.event_name == "cpu_frequency_limits" ||
      line.event_name == "cpu_idle" ||
      line.event_name == "binder_transaction" ||
      line.event_name == "binder_transaction_received" ||
      line.event_name == "binder_command" ||
      line.event_name == "binder_return" ||
      line.event_name == "binder_transaction_alloc_buf" ||
      line.event_name == "clock_set_rate" ||
      line.event_name == "clock_enable" ||
      line.event_name == "clock_disable" ||
      line.event_name == "thermal_temperature" ||
      line.event_name == "cdev_update" ||
      line.event_name == "sched_blocked_reason" ||
      line.event_name == "rss_stat";
  if (!needs_args) {
    return base::OkStatus();
  }

  struct SystraceArg {
    std::string_view key;
    std::string_view value;
  };
  base::SmallVector<SystraceArg, 16> args;

  std::string_view args_view(line.args_str);
  while (!args_view.empty()) {
    while (!args_view.empty() && args_view.front() == ' ') {
      args_view.remove_prefix(1);
    }
    if (args_view.empty())
      break;
    size_t space_pos = args_view.find(' ');
    std::string_view token = args_view.substr(0, space_pos);
    args_view.remove_prefix(space_pos == std::string_view::npos
                                ? args_view.size()
                                : space_pos);

    size_t eq_pos = token.find('=');
    if (eq_pos == std::string_view::npos) {
      args.emplace_back(SystraceArg{"name", token});
    } else {
      size_t last_eq = token.rfind('=');
      args.emplace_back(
          SystraceArg{token.substr(0, eq_pos), token.substr(last_eq + 1)});
    }
  }

  auto find_arg = [&](std::string_view key) -> std::optional<std::string_view> {
    for (size_t i = args.size(); i > 0; --i) {
      if (args[i - 1].key == key)
        return args[i - 1].value;
    }
    return std::nullopt;
  };

  if (line.event_name == "sched_switch") {
    auto prev_state_val = find_arg("prev_state").value_or("");
    char prev_state_buf[16];
    size_t prev_state_len =
        std::min(prev_state_val.size(), sizeof(prev_state_buf) - 1);
    memcpy(prev_state_buf, prev_state_val.data(), prev_state_len);
    prev_state_buf[prev_state_len] = '\0';
    int64_t prev_state =
        ftrace_utils::TaskState::FromSystrace(prev_state_buf)
            .ToRawStateOnlyForSystraceConversions();

    auto prev_pid = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("prev_pid").value_or("")));
    auto prev_comm =
        base::StringView(find_arg("prev_comm").value_or(""));
    auto prev_prio = base::StringViewToNumber<int32_t>(
        base::StringView(find_arg("prev_prio").value_or("")));
    auto next_pid = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("next_pid").value_or("")));
    auto next_comm =
        base::StringView(find_arg("next_comm").value_or(""));
    auto next_prio = base::StringViewToNumber<int32_t>(
        base::StringView(find_arg("next_prio").value_or("")));

    if (!(prev_pid.has_value() && prev_prio.has_value() &&
          next_pid.has_value() && next_prio.has_value())) {
      return base::ErrStatus("Could not parse sched_switch");
    }

    FtraceSchedEventTracker::GetOrCreate(context_)->PushSchedSwitch(
        line.cpu, line.ts, prev_pid.value(), prev_comm, prev_prio.value(),
        prev_state, next_pid.value(), next_comm, next_prio.value());
  } else if (line.event_name == "sched_waking") {
    auto comm = find_arg("comm").value_or("");
    std::optional<uint32_t> wakee_pid = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("pid").value_or("")));
    if (!wakee_pid.has_value()) {
      return base::ErrStatus("Could not convert wakee_pid");
    }

    StringId name_id = context_->storage->InternString(base::StringView(comm));
    auto wakee_utid =
        context_->process_tracker->GetOrCreateThread(wakee_pid.value());
    context_->process_tracker->UpdateThreadName(wakee_utid, name_id,
                                                ThreadNamePriority::kFtrace);

    ThreadStateTracker::GetOrCreate(context_)->PushWakingEvent(
        line.ts, wakee_utid, utid);

  } else if (line.event_name == "cpu_frequency") {
    std::optional<uint32_t> event_cpu = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("cpu_id").value_or("")));
    auto state_val = find_arg("state").value_or("");
    char state_buf[32];
    size_t state_len = std::min(state_val.size(), sizeof(state_buf) - 1);
    memcpy(state_buf, state_val.data(), state_len);
    state_buf[state_len] = '\0';
    std::optional<double> new_state = base::CStringToDouble(state_buf);
    if (!event_cpu.has_value()) {
      return base::ErrStatus("Could not convert event cpu");
    }
    if (!new_state.has_value()) {
      return base::ErrStatus("Could not convert state");
    }

    TrackId track = context_->track_tracker->InternTrack(
        tracks::kCpuFrequencyBlueprint, tracks::Dimensions(event_cpu.value()));
    context_->event_tracker->PushCounter(line.ts, new_state.value(), track);
  } else if (line.event_name == "cpu_frequency_limits") {
    std::optional<uint32_t> event_cpu = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("cpu_id").value_or("")));
    if (!event_cpu.has_value()) {
      return base::ErrStatus("Could not convert event cpu");
    }
    auto max_val = find_arg("max");
    if (!max_val)
      max_val = find_arg("max_freq");
    auto min_val = find_arg("min");
    if (!min_val)
      min_val = find_arg("min_freq");

    auto parse_double =
        [](std::optional<std::string_view> val) -> std::optional<double> {
      if (!val)
        return std::nullopt;
      char buf[32];
      size_t len = std::min(val->size(), sizeof(buf) - 1);
      memcpy(buf, val->data(), len);
      buf[len] = '\0';
      return base::CStringToDouble(buf);
    };

    std::optional<double> max_freq = parse_double(max_val);
    std::optional<double> min_freq = parse_double(min_val);
    if (max_freq.has_value()) {
      TrackId max_track = context_->track_tracker->InternTrack(
          tracks::kCpuMaxFrequencyLimitBlueprint,
          tracks::Dimensions(event_cpu.value()));
      context_->event_tracker->PushCounter(
          line.ts, static_cast<double>(max_freq.value()), max_track);
    }
    if (min_freq.has_value()) {
      TrackId min_track = context_->track_tracker->InternTrack(
          tracks::kCpuMinFrequencyLimitBlueprint,
          tracks::Dimensions(event_cpu.value()));
      context_->event_tracker->PushCounter(
          line.ts, static_cast<double>(min_freq.value()), min_track);
    }
    if (!max_freq.has_value() && !min_freq.has_value()) {
      return base::ErrStatus("Could not convert both max_freq and min_freq");
    }
  } else if (line.event_name == "cpu_idle") {
    std::optional<uint32_t> event_cpu = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("cpu_id").value_or("")));
    auto state_val = find_arg("state").value_or("");
    char state_buf[32];
    size_t state_len = std::min(state_val.size(), sizeof(state_buf) - 1);
    memcpy(state_buf, state_val.data(), state_len);
    state_buf[state_len] = '\0';
    std::optional<double> new_state = base::CStringToDouble(state_buf);
    if (!event_cpu.has_value()) {
      return base::ErrStatus("Could not convert event cpu");
    }
    if (!new_state.has_value()) {
      return base::ErrStatus("Could not convert state");
    }

    TrackId track = context_->track_tracker->InternTrack(
        tracks::kCpuIdleBlueprint, tracks::Dimensions(event_cpu.value()));
    context_->event_tracker->PushCounter(line.ts, new_state.value(), track);
  } else if (line.event_name == "binder_transaction") {
    auto id = base::StringViewToNumber<int32_t>(
        base::StringView(find_arg("transaction").value_or("")));
    auto dest_node = base::StringViewToNumber<int32_t>(
        base::StringView(find_arg("dest_node").value_or("")));
    auto dest_tgid = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("dest_proc").value_or("")));
    auto dest_tid = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("dest_thread").value_or("")));
    auto reply_opt = base::StringViewToNumber<int32_t>(
        base::StringView(find_arg("reply").value_or("")));
    auto is_reply = reply_opt.value_or(0) == 1;
    auto flags_sv = find_arg("flags").value_or("");
    char flags_buf[32];
    size_t flags_len = std::min(flags_sv.size(), sizeof(flags_buf) - 1);
    memcpy(flags_buf, flags_sv.data(), flags_len);
    flags_buf[flags_len] = '\0';
    char* end;
    uint32_t flags = static_cast<uint32_t>(strtol(flags_buf, &end, 16));
    std::string code_str =
        std::string(find_arg("code").value_or("")) + " Java Layer Dependent";
    StringId code = context_->storage->InternString(base::StringView(code_str));
    if (!dest_tgid.has_value()) {
      return base::ErrStatus("Could not convert dest_tgid");
    }
    if (!dest_tid.has_value()) {
      return base::ErrStatus("Could not convert dest_tid");
    }
    if (!id.has_value()) {
      return base::ErrStatus("Could not convert transaction id");
    }
    if (!dest_node.has_value()) {
      return base::ErrStatus("Could not convert dest node");
    }
    BinderTracker::GetOrCreate(context_)->Transaction(
        line.ts, line.pid, id.value(), dest_node.value(), dest_tgid.value(),
        dest_tid.value(), is_reply, flags, code);
  } else if (line.event_name == "binder_transaction_received") {
    auto id = base::StringViewToNumber<int32_t>(
        base::StringView(find_arg("transaction").value_or("")));
    if (!id.has_value()) {
      return base::ErrStatus("Could not convert transaction id");
    }
    BinderTracker::GetOrCreate(context_)->TransactionReceived(line.ts, line.pid,
                                                              id.value());
  } else if (line.event_name == "binder_command") {
    auto cmd_sv = find_arg("cmd").value_or("");
    char cmd_buf[32];
    size_t cmd_len = std::min(cmd_sv.size(), sizeof(cmd_buf) - 1);
    memcpy(cmd_buf, cmd_sv.data(), cmd_len);
    cmd_buf[cmd_len] = '\0';
    auto id = base::CStringToUInt32(cmd_buf, 0);
    if (!id.has_value()) {
      return base::ErrStatus("Could not convert cmd");
    }
    BinderTracker::GetOrCreate(context_)->CommandToKernel(line.ts, line.pid,
                                                          id.value());
  } else if (line.event_name == "binder_return") {
    auto cmd_sv = find_arg("cmd").value_or("");
    char cmd_buf[32];
    size_t cmd_len = std::min(cmd_sv.size(), sizeof(cmd_buf) - 1);
    memcpy(cmd_buf, cmd_sv.data(), cmd_len);
    cmd_buf[cmd_len] = '\0';
    auto id = base::CStringToUInt32(cmd_buf, 0);
    if (!id.has_value()) {
      return base::ErrStatus("Could not convert cmd");
    }
    BinderTracker::GetOrCreate(context_)->ReturnFromKernel(line.ts, line.pid,
                                                           id.value());
  } else if (line.event_name == "binder_transaction_alloc_buf") {
    auto data_size = base::StringViewToNumber<uint64_t>(
        base::StringView(find_arg("data_size").value_or("")));
    auto offsets_size = base::StringViewToNumber<uint64_t>(
        base::StringView(find_arg("offsets_size").value_or("")));
    if (!data_size.has_value()) {
      return base::ErrStatus("Could not convert data size");
    }
    if (!offsets_size.has_value()) {
      return base::ErrStatus("Could not convert offsets size");
    }
    BinderTracker::GetOrCreate(context_)->TransactionAllocBuf(
        line.ts, line.pid, data_size.value(), offsets_size.value());
  } else if (line.event_name == "clock_set_rate") {
    auto rate = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("state").value_or("")));
    if (!rate.has_value()) {
      return base::ErrStatus("Could not convert state");
    }
    TrackId track = context_->track_tracker->InternTrack(
        tracks::kClockFrequencyBlueprint,
        tracks::Dimensions(base::StringView(find_arg("name").value_or(""))));
    context_->event_tracker->PushCounter(line.ts, rate.value(), track);
  } else if (line.event_name == "clock_enable" ||
             line.event_name == "clock_disable") {
    auto rate = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("state").value_or("")));
    if (!rate.has_value()) {
      return base::ErrStatus("Could not convert state");
    }
    TrackId track = context_->track_tracker->InternTrack(
        tracks::kClockStateBlueprint,
        tracks::Dimensions(base::StringView(find_arg("name").value_or(""))));
    context_->event_tracker->PushCounter(line.ts, rate.value(), track);
  } else if (line.event_name == "thermal_temperature") {
    TrackId track = context_->track_tracker->InternTrack(
        tracks::kThermalTemperatureBlueprint,
        tracks::Dimensions(
            base::StringView(find_arg("thermal_zone").value_or(""))));
    auto temp = base::StringViewToNumber<int32_t>(
        base::StringView(find_arg("temp").value_or("")));
    if (!temp.has_value()) {
      return base::ErrStatus("Could not convert temp");
    }
    context_->event_tracker->PushCounter(line.ts, temp.value(), track);
  } else if (line.event_name == "cdev_update") {
    TrackId track = context_->track_tracker->InternTrack(
        tracks::kCoolingDeviceCounterBlueprint,
        tracks::Dimensions(base::StringView(find_arg("type").value_or(""))));
    auto target_val = find_arg("target").value_or("");
    char target_buf[32];
    size_t target_len = std::min(target_val.size(), sizeof(target_buf) - 1);
    memcpy(target_buf, target_val.data(), target_len);
    target_buf[target_len] = '\0';
    auto target = base::CStringToDouble(target_buf);
    if (!target.has_value()) {
      return base::ErrStatus("Could not convert target");
    }
    context_->event_tracker->PushCounter(line.ts, target.value(), track);
  } else if (line.event_name == "sched_blocked_reason") {
    auto wakee_pid = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("pid").value_or("")));
    if (!wakee_pid.has_value()) {
      return base::ErrStatus("sched_blocked_reason: could not parse wakee_pid");
    }
    auto wakee_utid = context_->process_tracker->GetOrCreateThread(*wakee_pid);
    auto io_wait = base::StringViewToNumber<int32_t>(
        base::StringView(find_arg("iowait").value_or("")));
    if (!io_wait.has_value()) {
      return base::ErrStatus("sched_blocked_reason: could not parse io_wait");
    }
    StringId blocked_function = context_->storage->InternString(
        base::StringView(find_arg("caller").value_or("")));
    ThreadStateTracker::GetOrCreate(context_)->PushBlockedReason(
        wakee_utid, static_cast<bool>(*io_wait), blocked_function);
  } else if (line.event_name == "rss_stat") {
    // Format: rss_stat: size=8437760 member=1 curr=1 mm_id=2824390453
    auto size = base::StringViewToNumber<int64_t>(
        base::StringView(find_arg("size").value_or("")));
    auto member = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("member").value_or("")));
    auto mm_id = base::StringViewToNumber<int64_t>(
        base::StringView(find_arg("mm_id").value_or("")));
    auto opt_curr = base::StringViewToNumber<uint32_t>(
        base::StringView(find_arg("curr").value_or("")));
    if (!size.has_value()) {
      return base::ErrStatus("rss_stat: could not parse size");
    }
    if (!member.has_value()) {
      return base::ErrStatus("rss_stat: could not parse member");
    }
    std::optional<bool> curr;
    if (opt_curr.has_value()) {
      curr = std::make_optional(static_cast<bool>(*opt_curr));
    }
    rss_stat_tracker_.ParseRssStat(line.ts, line.pid, *size, *member, curr,
                                   mm_id);
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
