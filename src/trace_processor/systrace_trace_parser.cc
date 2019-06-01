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

#include "src/trace_processor/systrace_trace_parser.h"

#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/systrace_utils.h"

#include <inttypes.h>
#include <string>
#include <unordered_map>

namespace perfetto {
namespace trace_processor {

namespace {

std::string SubstrTrim(const std::string& input, size_t start, size_t end) {
  auto s = input.substr(start, end - start);
  s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                                  [](int ch) { return !std::isspace(ch); }));
  s.erase(std::find_if(s.rbegin(), s.rend(),
                       [](int ch) { return !std::isspace(ch); })
              .base(),
          s.end());
  return s;
}

}  // namespace

SystraceTraceParser::SystraceTraceParser(TraceProcessorContext* ctx)
    : context_(ctx),
      sched_wakeup_name_id_(ctx->storage->InternString("sched_wakeup")),
      cpu_idle_name_id_(ctx->storage->InternString("cpuidle")) {}
SystraceTraceParser::~SystraceTraceParser() = default;

util::Status SystraceTraceParser::Parse(std::unique_ptr<uint8_t[]> owned_buf,
                                        size_t size) {
  if (state_ == ParseState::kEndOfSystrace)
    return util::OkStatus();
  partial_buf_.insert(partial_buf_.end(), &owned_buf[0], &owned_buf[size]);

  if (state_ == ParseState::kBeforeParse) {
    state_ = partial_buf_[0] == '<' ? ParseState::kHtmlBeforeSystrace
                                    : ParseState::kSystrace;
  }

  const char kSystraceStart[] =
      R"(<script class="trace-data" type="application/text">)";
  auto start_it = partial_buf_.begin();
  for (;;) {
    auto line_it = std::find(start_it, partial_buf_.end(), '\n');
    if (line_it == partial_buf_.end())
      break;

    std::string buffer(start_it, line_it);
    if (state_ == ParseState::kHtmlBeforeSystrace) {
      if (base::Contains(buffer, kSystraceStart)) {
        state_ = ParseState::kSystrace;
      }
    } else if (state_ == ParseState::kSystrace) {
      if (base::Contains(buffer, R"(</script>)")) {
        state_ = kEndOfSystrace;
        break;
      } else if (!base::StartsWith(buffer, "#")) {
        ParseSingleSystraceEvent(buffer);
      }
    }
    start_it = line_it + 1;
  }
  if (state_ == ParseState::kEndOfSystrace) {
    partial_buf_.clear();
  } else {
    partial_buf_.erase(partial_buf_.begin(), start_it);
  }
  return util::OkStatus();
}

util::Status SystraceTraceParser::ParseSingleSystraceEvent(
    const std::string& buffer) {
  // An example line from buffer looks something like the following:
  // <idle>-0     (-----) [000] d..1 16500.715638: cpu_idle: state=0 cpu_id=0

  auto task_idx = 16u;
  std::string task = SubstrTrim(buffer, 0, task_idx);

  auto tgid_idx = buffer.find('(', task_idx + 1);
  std::string pid_str = SubstrTrim(buffer, task_idx + 1, tgid_idx);
  auto pid = static_cast<uint32_t>(std::stoi(pid_str));
  context_->process_tracker->GetOrCreateThread(pid);

  auto tgid_end = buffer.find(')', tgid_idx + 1);
  std::string tgid_str = SubstrTrim(buffer, tgid_idx + 1, tgid_end);
  auto tgid = tgid_str == "-----"
                  ? base::nullopt
                  : base::Optional<uint32_t>(
                        static_cast<uint32_t>(std::stoi(tgid_str)));
  if (tgid.has_value()) {
    context_->process_tracker->UpdateThread(pid, tgid.value());
  }

  auto cpu_idx = buffer.find('[', tgid_end + 1);
  auto cpu_end = buffer.find(']', cpu_idx + 1);
  std::string cpu_str = SubstrTrim(buffer, cpu_idx + 1, cpu_end);
  auto cpu = static_cast<uint32_t>(std::stoi(cpu_str));

  auto ts_idx = buffer.find(' ', cpu_end + 2);
  auto ts_end = buffer.find(':', ts_idx + 1);
  std::string ts_str = SubstrTrim(buffer, ts_idx + 1, ts_end);
  auto ts_float = std::stod(ts_str) * 1e9;
  auto ts = static_cast<int64_t>(ts_float);

  auto fn_idx = buffer.find(':', ts_end + 2);
  std::string fn = SubstrTrim(buffer, ts_end + 2, fn_idx);

  std::string args_str = SubstrTrim(buffer, fn_idx + 2, buffer.size());

  std::unordered_map<std::string, std::string> args;
  for (base::StringSplitter ss(args_str.c_str(), ' '); ss.Next();) {
    std::string key;
    std::string value;
    for (base::StringSplitter inner(ss.cur_token(), '='); inner.Next();) {
      if (key.empty()) {
        key = inner.cur_token();
      } else {
        value = inner.cur_token();
      }
    }
    args.emplace(std::move(key), std::move(value));
  }
  if (fn == "sched_switch") {
    auto prev_state_str = args["prev_state"];
    int64_t prev_state =
        ftrace_utils::TaskState(prev_state_str.c_str()).raw_state();

    auto prev_pid = std::stoi(args["prev_pid"]);
    auto prev_comm = base::StringView(args["prev_comm"]);
    auto prev_prio = std::stoi(args["prev_prio"]);
    auto next_pid = std::stoi(args["next_pid"]);
    auto next_comm = base::StringView(args["next_comm"]);
    auto next_prio = std::stoi(args["next_prio"]);

    context_->event_tracker->PushSchedSwitch(
        static_cast<uint32_t>(cpu), ts, static_cast<uint32_t>(prev_pid),
        prev_comm, prev_prio, prev_state, static_cast<uint32_t>(next_pid),
        next_comm, next_prio);
  } else if (fn == "tracing_mark_write") {
    systrace_utils::SystraceTracePoint point;
    auto result = ParseSystraceTracePoint(args_str.c_str(), &point);
    if (result == systrace_utils::SystraceParseResult::kSuccess) {
      switch (point.phase) {
        case 'B': {
          StringId name_id = context_->storage->InternString(point.name);
          context_->slice_tracker->BeginAndroid(ts, pid, point.tgid,
                                                0 /*cat_id*/, name_id);
          break;
        }
        case 'E': {
          context_->slice_tracker->EndAndroid(ts, pid, point.tgid);
          break;
        }
        case 'C': {
          // This is per upid on purpose. Some counters are pushed from
          // arbitrary threads but are really per process.
          UniquePid upid =
              context_->process_tracker->GetOrCreateProcess(point.tgid);
          StringId name_id = context_->storage->InternString(point.name);
          context_->event_tracker->PushCounter(ts, point.value, name_id, upid,
                                               RefType::kRefUpid);
        }
      }
    }
  } else if (fn == "sched_wakeup") {
    auto comm = args["comm"];
    uint32_t wakee_pid = static_cast<uint32_t>(std::stoi(args["pid"]));

    StringId name_id = context_->storage->InternString(base::StringView(comm));
    auto wakee_utid =
        context_->process_tracker->UpdateThreadName(wakee_pid, name_id);
    context_->event_tracker->PushInstant(ts, sched_wakeup_name_id_,
                                         0 /* value */, wakee_utid,
                                         RefType::kRefUtid);
  } else if (fn == "cpu_idle") {
    auto new_state = static_cast<double>(std::stol(args["state"]));
    uint32_t event_cpu = static_cast<uint32_t>(std::stoi(args["cpu_id"]));
    context_->event_tracker->PushCounter(ts, new_state, cpu_idle_name_id_,
                                         event_cpu, RefType::kRefCpuId);
  }

  return util::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
