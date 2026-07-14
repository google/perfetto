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

#include "src/trace_processor/plugins/strace/strace_trace_tokenizer.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/plugins/strace/strace_event.h"
#include "src/trace_processor/plugins/strace/strace_trace_parser.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/clock_synchronizer.h"

namespace perfetto::trace_processor::strace_importer {

namespace {

constexpr int64_t kNsPerSec = 1000LL * 1000 * 1000;
constexpr int64_t kNsPerUs = 1000;

std::string_view ToStringView(const TraceBlobView& tbv) {
  return {reinterpret_cast<const char*>(tbv.data()), tbv.size()};
}

// Parses a `-ttt` "seconds[.microseconds]" timestamp (Unix epoch) into
// nanoseconds. Returns std::nullopt for anything else, including `-t`/`-tt`
// "HH:MM:SS[.ffffff]" time-of-day timestamps: those contain no date and
// cannot be safely reinterpreted as an absolute point in time (see
// StraceLine's header comment), so callers must reject them rather than
// silently treat the pre-midnight offset as an epoch time.
std::optional<int64_t> ParseEpochTimestamp(std::string_view s) {
  if (s.find(':') != std::string_view::npos) {
    return std::nullopt;
  }

  size_t dot = s.find('.');
  std::string_view whole = s.substr(0, dot);
  std::string_view frac =
      dot == std::string_view::npos ? std::string_view() : s.substr(dot + 1);

  auto seconds = base::StringToInt64(std::string(whole));
  if (!seconds)
    return std::nullopt;

  int64_t ns = *seconds * kNsPerSec;
  if (!frac.empty()) {
    auto us = base::StringToInt64(std::string(frac));
    if (!us)
      return std::nullopt;
    // frac is microseconds regardless of how many digits strace prints.
    ns += *us * kNsPerUs;
  }
  return ns;
}

}  // namespace

std::optional<StraceLine> ParseStraceLine(std::string_view line) {
  std::string_view rest = base::TrimWhitespace(line);
  if (rest.empty() || rest[0] == '-' /* "--- SIGCHLD ... ---" */ ||
      rest[0] == '+' /* "+++ exited with 0 +++" */) {
    return std::nullopt;
  }

  StraceLine out;

  // Optional leading pid (present with -f/-ff): "1234 1700000000.000000 ...".
  {
    size_t sp = rest.find(' ');
    if (sp != std::string_view::npos) {
      std::string_view head = rest.substr(0, sp);
      bool all_digits = !head.empty();
      for (char c : head) {
        if (!isdigit(static_cast<unsigned char>(c))) {
          all_digits = false;
          break;
        }
      }
      if (all_digits) {
        auto pid = base::StringToUInt32(std::string(head));
        if (!pid)
          return std::nullopt;
        out.pid = *pid;
        rest = base::TrimWhitespace(rest.substr(sp + 1));
      }
    }
  }

  // Timestamp: a run of digits optionally followed by ".digits", ending at
  // the next space. Only the -ttt (Unix epoch) form is accepted; see
  // ParseEpochTimestamp.
  {
    size_t sp = rest.find(' ');
    if (sp == std::string_view::npos)
      return std::nullopt;
    std::string_view ts_tok = rest.substr(0, sp);
    auto ts = ParseEpochTimestamp(ts_tok);
    if (!ts)
      return std::nullopt;
    out.epoch_ns = *ts;
    rest = base::TrimWhitespace(rest.substr(sp + 1));
  }

  if (rest.empty())
    return std::nullopt;

  // "<... syscall resumed> rest-of-args) = ret"
  constexpr std::string_view kResumedPrefix = "<... ";
  if (rest.substr(0, kResumedPrefix.size()) == kResumedPrefix) {
    size_t name_start = kResumedPrefix.size();
    size_t name_end = rest.find(" resumed>", name_start);
    if (name_end == std::string_view::npos)
      return std::nullopt;
    out.syscall = std::string(rest.substr(name_start, name_end - name_start));
    out.is_resumed = true;
    rest = base::TrimWhitespace(
        rest.substr(name_end + std::string_view(" resumed>").size()));
  } else {
    size_t paren = rest.find('(');
    if (paren == std::string_view::npos)
      return std::nullopt;
    out.syscall = std::string(rest.substr(0, paren));
    rest = rest.substr(paren + 1);
  }

  // "<unfinished ...>" has no closing paren/return value.
  constexpr std::string_view kUnfinished = "<unfinished ...>";
  size_t unfinished_pos = rest.find(kUnfinished);
  if (unfinished_pos != std::string_view::npos) {
    out.args =
        std::string(base::TrimWhitespace(rest.substr(0, unfinished_pos)));
    // Strip a trailing comma left over from "arg1, arg2, <unfinished ...>".
    while (!out.args.empty() && (out.args.back() == ',')) {
      out.args.pop_back();
    }
    out.is_unfinished = true;
    return out;
  }

  // Otherwise this is a complete call: "...) = <return>". The return value
  // itself may contain further parens (e.g. "-1 ENOENT (No such file or
  // directory)"), so anchor on the literal ") = " marker that separates the
  // call's own closing paren from its return value, rather than the last
  // ')' in the line.
  constexpr std::string_view kCloseEq = ") = ";
  size_t close_paren = rest.find(kCloseEq);
  if (close_paren == std::string_view::npos) {
    // A resumed line whose original call had no args left, e.g.
    // "<... read resumed>) = 3" collapses to just "= 3" after we've already
    // stripped the leading ")" as part of the resumed-prefix parsing above.
    size_t eq = rest.find('=');
    if (eq == std::string_view::npos)
      return std::nullopt;
    out.return_value = std::string(base::TrimWhitespace(rest.substr(eq + 1)));
    return out;
  }

  out.args = std::string(base::TrimWhitespace(rest.substr(0, close_paren)));
  out.return_value = std::string(
      base::TrimWhitespace(rest.substr(close_paren + kCloseEq.size())));
  return out;
}

bool IsStraceFormatTrace(const uint8_t* ptr, size_t size) {
  std::string_view str(reinterpret_cast<const char*>(ptr), size);
  size_t nl = str.find('\n');
  std::string_view first_line =
      nl == std::string_view::npos ? str : str.substr(0, nl);
  return ParseStraceLine(first_line).has_value();
}

StraceTraceTokenizer::StraceTraceTokenizer(TraceProcessorContext* ctx)
    : context_(ctx),
      stream_(
          ctx->sorter->CreateStream(std::make_unique<StraceTraceParser>(ctx))) {
}
StraceTraceTokenizer::~StraceTraceTokenizer() = default;

base::Status StraceTraceTokenizer::Parse(TraceBlobView blob) {
  reader_.PushBack(std::move(blob));
  for (;;) {
    auto it = reader_.GetIterator();
    auto r = it.MaybeFindAndRead('\n');
    if (!r) {
      return base::OkStatus();
    }
    std::string_view line = ToStringView(*r);
    reader_.PopFrontUntil(it.file_offset());

    std::optional<StraceLine> parsed = ParseStraceLine(line);
    if (!parsed) {
      // Not every line in an strace log is a syscall (signal delivery,
      // process exit banners, a `-t`/`-tt` timestamp we intentionally don't
      // support, etc). Count it and skip rather than treating the whole
      // trace as invalid.
      context_->stats_tracker->IncrementStats(stats::strace_parse_failure);
      continue;
    }

    std::optional<int64_t> trace_ts =
        context_->clock_tracker->ConvertDefaultClockToTraceTime(
            parsed->epoch_ns);
    if (!trace_ts) {
      continue;
    }

    StraceEvent evt;
    evt.tid = parsed->pid.value_or(1);
    evt.syscall_name_id =
        context_->storage->InternString(base::StringView(parsed->syscall));
    if (!parsed->args.empty()) {
      evt.args_id =
          context_->storage->InternString(base::StringView(parsed->args));
    }
    if (parsed->return_value) {
      evt.return_value_id = context_->storage->InternString(
          base::StringView(*parsed->return_value));
    }
    evt.is_unfinished = parsed->is_unfinished;
    evt.is_resumed = parsed->is_resumed;

    stream_->Push(*trace_ts, evt);
  }
}

}  // namespace perfetto::trace_processor::strace_importer
