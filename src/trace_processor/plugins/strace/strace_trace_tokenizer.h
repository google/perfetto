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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_STRACE_STRACE_TRACE_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_STRACE_STRACE_TRACE_TOKENIZER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "perfetto/base/status.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/plugins/strace/strace_event.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto::trace_processor::strace_importer {

// The parsed representation of a single line of `strace -ttt` output.
//
// Examples of lines this is built from:
//   1700000000.000000 openat(AT_FDCWD, "/etc/passwd", O_RDONLY) = 3
//   1700000000.123456 read(3, "root:x:0:0"..., 1024) = 312
//   1234 1700000000.000000 read(3, "root:x:0:0"..., 1024) = 312    (-f)
//   1700000000.000000 read(3,  <unfinished ...>
//   1700000000.000500 <... read resumed> "root:x:0:0"..., 1024) = 312
//
// Only `-ttt` (Unix epoch seconds, with an optional fractional part) is
// supported. `-t`/`-tt` print wall-clock time-of-day with no date, which
// cannot be safely treated as an absolute point in time: it wraps at
// midnight and, if fed as-is into the realtime clock domain, silently
// produces nonsensical (epoch-relative-to-1970) timestamps when the trace
// is merged with any other clock-synchronized trace. Rather than get this
// subtly wrong, `-t`/`-tt` lines are rejected outright (see
// stats::strace_parse_failure).
struct StraceLine {
  // Unix epoch nanoseconds, from `-ttt`'s "seconds.microseconds" timestamp.
  int64_t epoch_ns = 0;
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

// Parses a single line (no trailing newline) of strace -ttt output. Returns
// std::nullopt if the line doesn't look like strace -ttt output at all —
// e.g. a signal delivery line like "--- SIGCHLD {...} ---", a process exit
// line, or a `-t`/`-tt` timestamp (see StraceLine's comment on why those
// are rejected rather than misinterpreted); callers should count such lines
// via stats::strace_parse_failure rather than treat them as a hard error.
std::optional<StraceLine> ParseStraceLine(std::string_view line);

// Sniffs whether the first line of a (possibly truncated) buffer looks like
// strace -ttt output, for trace-type auto-detection.
bool IsStraceFormatTrace(const uint8_t* ptr, size_t size);

class StraceTraceTokenizer : public ChunkedTraceReader {
 public:
  explicit StraceTraceTokenizer(TraceProcessorContext*);
  ~StraceTraceTokenizer() override;

  base::Status Parse(TraceBlobView) override;
  base::Status OnPushDataToSorter() override { return base::OkStatus(); }
  void OnEventsFullyExtracted() override {}

 private:
  TraceProcessorContext* const context_;
  util::TraceBlobViewReader reader_;
  std::unique_ptr<TraceSorter::Stream<StraceEvent>> stream_;
};

}  // namespace perfetto::trace_processor::strace_importer

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_STRACE_STRACE_TRACE_TOKENIZER_H_
