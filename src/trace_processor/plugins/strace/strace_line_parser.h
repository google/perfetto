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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_STRACE_STRACE_LINE_PARSER_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_STRACE_STRACE_LINE_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace perfetto::trace_processor::strace_importer {

// The parsed representation of a single line of `strace -t` (or -tt/-ttt)
// output.
//
// Examples of lines this is built from:
//   14:32:01 openat(AT_FDCWD, "/etc/passwd", O_RDONLY) = 3
//   14:32:01.123456 read(3, "root:x:0:0"..., 1024) = 312
//   1234 14:32:01 read(3, "root:x:0:0"..., 1024) = 312          (-f)
//   14:32:01 read(3,  <unfinished ...>
//   14:32:01 <... read resumed> "root:x:0:0"..., 1024) = 312
struct StraceLine {
  // Wall-clock time of day the line was emitted, as nanoseconds since
  // midnight. strace -t has second resolution, -tt/-ttt add microseconds.
  int64_t tod_ns = 0;
  // Present only when the trace was collected with `strace -f`/`-ff`.
  std::optional<uint32_t> pid;
  // The syscall name, e.g. "openat". Empty for a "<... foo resumed>" line
  // (the name is recovered from the matching unfinished call).
  std::string syscall;
  // The raw text between the syscall name's parentheses (or, for a resumed
  // line, whatever text follows "resumed>").
  std::string args;
  // The text after "= " at the end of a *complete* call, e.g. "3" or "-1
  // ENOENT (No such file or directory)". Unset for unfinished calls.
  std::optional<std::string> return_value;
  bool is_unfinished = false;
  bool is_resumed = false;
};

// Parses a single line (no trailing newline) of strace -t output. Returns
// std::nullopt if the line doesn't look like strace output at all (e.g. a
// signal delivery line like "--- SIGCHLD {...} ---" or a process exit line);
// callers should skip such lines rather than treat them as a hard error.
std::optional<StraceLine> ParseStraceLine(std::string_view line);

// Sniffs whether the first line of a (possibly truncated) buffer looks like
// strace -t output, for trace-type auto-detection.
bool IsStraceFormatTrace(const uint8_t* ptr, size_t size);

}  // namespace perfetto::trace_processor::strace_importer

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_STRACE_STRACE_LINE_PARSER_H_
