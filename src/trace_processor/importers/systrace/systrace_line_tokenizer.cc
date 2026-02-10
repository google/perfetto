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

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/systrace/systrace_line.h"

namespace perfetto::trace_processor {

namespace {

std::string_view TrimWhitespace(std::string_view sv) {
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
    sv.remove_prefix(1);
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
    sv.remove_suffix(1);
  return sv;
}

}  // namespace

SystraceLineTokenizer::SystraceLineTokenizer() = default;

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
// The task name can contain any characters e.g -:[(/ so we anchor the
// parse on the CPU bracket [<digits>] and work outwards from there.
base::Status SystraceLineTokenizer::Tokenize(const std::string& buffer,
                                             SystraceLine* line) {
  const char* buf = buffer.data();
  const size_t len = buffer.size();

  // Step 1: Find the CPU bracket [<digits>]. This is the most reliable
  // anchor point in the line format.
  size_t cpu_open = std::string::npos;
  size_t cpu_close = std::string::npos;
  for (size_t i = 0; i < len; ++i) {
    if (buf[i] != '[')
      continue;
    size_t j = i + 1;
    while (j < len && std::isdigit(static_cast<unsigned char>(buf[j])))
      ++j;
    if (j > i + 1 && j < len && buf[j] == ']') {
      cpu_open = i;
      cpu_close = j;
      break;
    }
  }
  if (cpu_open == std::string::npos) {
    return base::ErrStatus("Not a known systrace event format (line: %s)",
                           buffer.c_str());
  }

  std::string_view cpu_sv(buf + cpu_open + 1, cpu_close - cpu_open - 1);

  // Step 2: Parse backwards from '[' for tgid, pid, and task name.
  size_t pos = cpu_open;

  // Skip whitespace backwards.
  while (pos > 0 && std::isspace(static_cast<unsigned char>(buf[pos - 1])))
    --pos;

  // Optional tgid in parens: ( <tgid>) or (<tgid>).
  std::string_view tgid_sv;
  if (pos > 0 && buf[pos - 1] == ')') {
    --pos;  // skip ')'
    size_t paren_end = pos;
    while (pos > 0 && buf[pos - 1] != '(')
      --pos;
    if (pos == 0) {
      return base::ErrStatus("Not a known systrace event format (line: %s)",
                             buffer.c_str());
    }
    tgid_sv = TrimWhitespace(std::string_view(buf + pos, paren_end - pos));
    --pos;  // skip '('
    // Skip whitespace backwards.
    while (pos > 0 && std::isspace(static_cast<unsigned char>(buf[pos - 1])))
      --pos;
  }

  // Scan backwards through digits for the pid.
  size_t pid_end = pos;
  while (pos > 0 && std::isdigit(static_cast<unsigned char>(buf[pos - 1])))
    --pos;
  if (pos == pid_end || pos == 0 || buf[pos - 1] != '-') {
    return base::ErrStatus("Not a known systrace event format (line: %s)",
                           buffer.c_str());
  }
  std::string_view pid_sv(buf + pos, pid_end - pos);
  --pos;  // skip '-'

  // Everything before that '-' is the task name.
  std::string_view task_sv = TrimWhitespace(std::string_view(buf, pos));

  // Step 3: Parse forwards from after ']' for timestamp, event name, and args.
  // Skip irq flags (if present) and whitespace by scanning for the first
  // <digits>.<digits>: pattern which is the timestamp.
  pos = cpu_close + 1;

  size_t ts_start = std::string::npos;
  size_t ts_end = std::string::npos;
  for (size_t i = pos; i < len; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(buf[i])))
      continue;
    size_t j = i;
    while (j < len && std::isdigit(static_cast<unsigned char>(buf[j])))
      ++j;
    if (j < len && buf[j] == '.') {
      size_t dot = j;
      ++j;
      while (j < len && std::isdigit(static_cast<unsigned char>(buf[j])))
        ++j;
      if (j > dot + 1 && j < len && buf[j] == ':') {
        ts_start = i;
        ts_end = j;
        break;
      }
    }
  }
  if (ts_start == std::string::npos) {
    return base::ErrStatus("Not a known systrace event format (line: %s)",
                           buffer.c_str());
  }

  std::string_view ts_sv(buf + ts_start, ts_end - ts_start);

  // After the timestamp ':', skip whitespace, then read the event name
  // (non-whitespace until ':').
  pos = ts_end + 1;
  while (pos < len && std::isspace(static_cast<unsigned char>(buf[pos])))
    ++pos;
  size_t event_start = pos;
  while (pos < len && buf[pos] != ':' &&
         !std::isspace(static_cast<unsigned char>(buf[pos])))
    ++pos;
  if (pos >= len || buf[pos] != ':') {
    return base::ErrStatus("Not a known systrace event format (line: %s)",
                           buffer.c_str());
  }
  std::string_view event_sv(buf + event_start, pos - event_start);
  ++pos;  // skip ':'

  std::string_view args_sv =
      TrimWhitespace(std::string_view(buf + pos, len - pos));

  // Step 4: Convert and populate the SystraceLine.
  line->task = std::string(task_sv);
  line->tgid_str = std::string(tgid_sv);
  line->event_name = std::string(event_sv);
  line->args_str = std::string(args_sv);

  std::optional<uint32_t> maybe_pid =
      base::StringToUInt32(std::string(pid_sv));
  if (!maybe_pid.has_value()) {
    return base::ErrStatus("Could not convert pid %.*s",
                           static_cast<int>(pid_sv.size()), pid_sv.data());
  }
  line->pid = *maybe_pid;

  std::optional<uint32_t> maybe_cpu =
      base::StringToUInt32(std::string(cpu_sv));
  if (!maybe_cpu.has_value()) {
    return base::ErrStatus("Could not convert cpu %.*s",
                           static_cast<int>(cpu_sv.size()), cpu_sv.data());
  }
  line->cpu = *maybe_cpu;

  std::optional<double> maybe_ts = base::StringToDouble(std::string(ts_sv));
  if (!maybe_ts.has_value()) {
    return base::ErrStatus("Could not convert ts %.*s",
                           static_cast<int>(ts_sv.size()), ts_sv.data());
  }
  line->ts = static_cast<int64_t>(maybe_ts.value() * 1e9);

  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
