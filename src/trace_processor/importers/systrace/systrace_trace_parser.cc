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

#include "src/trace_processor/importers/systrace/systrace_trace_parser.h"

#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/importers/ftrace/sched_event_tracker.h"
#include "src/trace_processor/importers/systrace/systrace_parser.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/track_tracker.h"

#include <inttypes.h>
#include <string>
#include <unordered_map>

namespace perfetto {
namespace trace_processor {

namespace {
std::string SubstrTrim(const std::string& input) {
  std::string s = input;
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
      cpu_idle_name_id_(ctx->storage->InternString("cpuidle")),
      line_matcher_(std::regex(R"(-(\d+)\s+\(?\s*(\d+|-+)?\)?\s?\[(\d+)\]\s*)"
                               R"([a-zA-Z0-9.]{0,4}\s+(\d+\.\d+):\s+(\S+):)")) {
}
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

  // There can be multiple trace data sections in an HTML trace, we want to
  // ignore any that don't contain systrace data. In the future it would be
  // good to also parse the process dump section.
  const char kTraceDataSection[] =
      R"(<script class="trace-data" type="application/text">)";
  auto start_it = partial_buf_.begin();
  for (;;) {
    auto line_it = std::find(start_it, partial_buf_.end(), '\n');
    if (line_it == partial_buf_.end())
      break;

    std::string buffer(start_it, line_it);

    if (state_ == ParseState::kHtmlBeforeSystrace) {
      if (base::Contains(buffer, kTraceDataSection)) {
        state_ = ParseState::kTraceDataSection;
      }
    } else if (state_ == ParseState::kTraceDataSection) {
      if (base::StartsWith(buffer, "#")) {
        state_ = ParseState::kSystrace;
      } else if (base::Contains(buffer, R"(</script>)")) {
        state_ = ParseState::kHtmlBeforeSystrace;
      }
    } else if (state_ == ParseState::kSystrace) {
      if (base::Contains(buffer, R"(</script>)")) {
        state_ = ParseState::kEndOfSystrace;
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

// TODO(hjd): This should be more robust to being passed random input.
// This can happen if we mess up detecting a gzip trace for example.
util::Status SystraceTraceParser::ParseSingleSystraceEvent(
    const std::string& buffer) {
  // An example line from buffer looks something like the following:
  // kworker/u16:1-77    (   77) [004] ....   316.196720: 0:
  // B|77|__scm_call_armv8_64|0
  //
  // However, sometimes the tgid can be missing and buffer looks like this:
  // <idle>-0     [000] ...2     0.002188: task_newtask: pid=1 ...
  //
  // Also the irq fields can be missing (we don't parse these anyway)
  // <idle>-0     [000]  0.002188: task_newtask: pid=1 ...
  //
  // The task name can contain any characters e.g -:[(/ and for this reason
  // it is much easier to use a regex (even though it is slower than parsing
  // manually)

  std::smatch matches;
  bool matched = std::regex_search(buffer, matches, line_matcher_);
  if (!matched) {
    return util::Status("Not a known systrace event format");
  }

  std::string task = SubstrTrim(matches.prefix());
  std::string pid_str = matches[1].str();
  std::string tgid_str = matches[2].str();
  std::string cpu_str = matches[3].str();
  std::string ts_str = matches[4].str();
  std::string event_name = matches[5].str();
  std::string args_str = SubstrTrim(matches.suffix());

  base::Optional<uint32_t> maybe_pid = base::StringToUInt32(pid_str);
  if (!maybe_pid.has_value()) {
    return util::Status("Could not convert pid " + pid_str);
  }
  uint32_t pid = maybe_pid.value();
  context_->process_tracker->GetOrCreateThread(pid);

  if (tgid_str != "" && tgid_str != "-----") {
    base::Optional<uint32_t> tgid = base::StringToUInt32(tgid_str);
    if (tgid) {
      context_->process_tracker->UpdateThread(pid, tgid.value());
    }
  }

  base::Optional<uint32_t> maybe_cpu = base::StringToUInt32(cpu_str);
  if (!maybe_cpu.has_value()) {
    return util::Status("Could not convert cpu " + cpu_str);
  }
  uint32_t cpu = maybe_cpu.value();

  base::Optional<double> maybe_ts = base::StringToDouble(ts_str);
  if (!maybe_ts.has_value()) {
    return util::Status("Could not convert ts");
  }
  int64_t ts = static_cast<int64_t>(maybe_ts.value() * 1e9);

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
  if (event_name == "sched_switch") {
    auto prev_state_str = args["prev_state"];
    int64_t prev_state =
        ftrace_utils::TaskState(prev_state_str.c_str()).raw_state();

    auto prev_pid = base::StringToUInt32(args["prev_pid"]);
    auto prev_comm = base::StringView(args["prev_comm"]);
    auto prev_prio = base::StringToInt32(args["prev_prio"]);
    auto next_pid = base::StringToUInt32(args["next_pid"]);
    auto next_comm = base::StringView(args["next_comm"]);
    auto next_prio = base::StringToInt32(args["next_prio"]);

    if (!(prev_pid.has_value() && prev_prio.has_value() &&
          next_pid.has_value() && next_prio.has_value())) {
      return util::Status("Could not parse sched_switch");
    }

    context_->sched_tracker->PushSchedSwitch(
        cpu, ts, prev_pid.value(), prev_comm, prev_prio.value(), prev_state,
        next_pid.value(), next_comm, next_prio.value());
  } else if (event_name == "tracing_mark_write" || event_name == "0" ||
             event_name == "print") {
    context_->systrace_parser->ParsePrintEvent(ts, pid, args_str.c_str());
  } else if (event_name == "sched_wakeup") {
    auto comm = args["comm"];
    base::Optional<uint32_t> wakee_pid = base::StringToUInt32(args["pid"]);
    if (!wakee_pid.has_value()) {
      return util::Status("Could not convert wakee_pid");
    }

    StringId name_id = context_->storage->InternString(base::StringView(comm));
    auto wakee_utid =
        context_->process_tracker->UpdateThreadName(wakee_pid.value(), name_id);
    context_->event_tracker->PushInstant(ts, sched_wakeup_name_id_,
                                         0 /* value */, wakee_utid,
                                         RefType::kRefUtid);
  } else if (event_name == "cpu_idle") {
    base::Optional<uint32_t> event_cpu = base::StringToUInt32(args["cpu_id"]);
    base::Optional<double> new_state = base::StringToDouble(args["state"]);
    if (!event_cpu.has_value()) {
      return util::Status("Could not convert event cpu");
    }
    if (!event_cpu.has_value()) {
      return util::Status("Could not convert state");
    }

    TrackId track = context_->track_tracker->InternCpuCounterTrack(
        cpu_idle_name_id_, event_cpu.value());
    context_->event_tracker->PushCounter(ts, new_state.value(), track);
  }

  return util::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
