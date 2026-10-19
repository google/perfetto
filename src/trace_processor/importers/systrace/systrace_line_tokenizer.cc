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

#include "src/trace_processor/importers/systrace/systrace_line_tokenizer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/systrace/systrace_line.h"

namespace perfetto::trace_processor {

namespace {
inline std::string_view TrimWhitespace(std::string_view sv) {
  while (!sv.empty() && base::IsSpace(sv.front())) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && base::IsSpace(sv.back())) {
    sv.remove_suffix(1);
  }
  return sv;
}
}  // namespace

SystraceLineTokenizer::SystraceLineTokenizer()
    : line_matcher_(base::Regex::CreateOrCheck(
          R"(-(\d+)\s+\(?\s*(\d+|-+)?\)?\s?\[(\d+)\]\s*([a-zA-Z.]{3}[0-9a-f.]{1,2}\s*)?(\d+\.\d+):\s+(\S+):)")) {
  matches_.reserve(8);
}

// TODO(hjd): This should be more robust to being passed random input.
// This can happen if we mess up detecting a gzip trace for example.
base::Status SystraceLineTokenizer::Tokenize(std::string_view buffer,
                                             SystraceLine* line) {
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

  matches_.clear();
  if (!line_matcher_.PartialMatchWithGroups(buffer, matches_)) {
    return base::ErrStatus("Not a known systrace event format (line: %.*s)",
                           static_cast<int>(buffer.size()), buffer.data());
  }

  std::string_view pid_sv = matches_[1];
  std::string_view cpu_sv = matches_[3];
  std::string_view ts_sv = matches_[5];

  std::string_view prefix(
      buffer.data(), static_cast<size_t>(matches_[0].data() - buffer.data()));

  const char* match_end = matches_[0].data() + matches_[0].size();
  std::string_view suffix(
      match_end,
      static_cast<size_t>(buffer.data() + buffer.size() - match_end));

  std::string_view trimmed_prefix = TrimWhitespace(prefix);
  std::string_view trimmed_suffix = TrimWhitespace(suffix);

  line->task.assign(trimmed_prefix.data(), trimmed_prefix.size());
  line->tgid_str.assign(matches_[2].data(), matches_[2].size());
  line->event_name.assign(matches_[6].data(), matches_[6].size());
  line->args_str.assign(trimmed_suffix.data(), trimmed_suffix.size());

  std::optional<uint32_t> maybe_pid =
      base::StringViewToNumber<uint32_t>(base::StringView(pid_sv.data(), pid_sv.size()));
  if (!maybe_pid.has_value()) {
    return base::ErrStatus("Could not convert pid %.*s",
                           static_cast<int>(pid_sv.size()), pid_sv.data());
  }
  line->pid = maybe_pid.value();

  std::optional<uint32_t> maybe_cpu =
      base::StringViewToNumber<uint32_t>(base::StringView(cpu_sv.data(), cpu_sv.size()));
  if (!maybe_cpu.has_value()) {
    return base::ErrStatus("Could not convert cpu %.*s",
                           static_cast<int>(cpu_sv.size()), cpu_sv.data());
  }
  line->cpu = maybe_cpu.value();

  char ts_buf[32];
  if (PERFETTO_UNLIKELY(ts_sv.size() >= sizeof(ts_buf))) {
    return base::ErrStatus("Timestamp too long in systrace line");
  }
  memcpy(ts_buf, ts_sv.data(), ts_sv.size());
  ts_buf[ts_sv.size()] = '\0';

  std::optional<double> maybe_ts = base::CStringToDouble(ts_buf);
  if (!maybe_ts.has_value()) {
    return base::ErrStatus("Could not convert ts %s", ts_buf);
  }
  line->ts = static_cast<int64_t>(maybe_ts.value() * 1e9);

  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
