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

#include "src/trace_processor/plugins/strace/strace_line_parser.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "perfetto/ext/base/string_utils.h"

namespace perfetto::trace_processor::strace_importer {

namespace {

constexpr int64_t kNsPerSec = 1000LL * 1000 * 1000;
constexpr int64_t kNsPerUs = 1000;

// Parses a "HH:MM:SS", "HH:MM:SS.ffffff" (-tt) timestamp into nanoseconds
// since midnight. "-ttt" (Unix epoch seconds.ffffff) is also accepted here:
// callers only care about a monotonically increasing offset, and epoch
// seconds increase just as well as time-of-day seconds do.
std::optional<int64_t> ParseTimeOfDay(std::string_view s) {
  size_t dot = s.find('.');
  std::string_view whole = s.substr(0, dot);
  std::string_view frac =
      dot == std::string_view::npos ? std::string_view() : s.substr(dot + 1);

  int64_t seconds;
  size_t c1 = whole.find(':');
  if (c1 == std::string_view::npos) {
    // -ttt: plain Unix epoch seconds, no colons.
    auto v = base::StringToInt64(std::string(whole));
    if (!v)
      return std::nullopt;
    seconds = *v;
  } else {
    size_t c2 = whole.find(':', c1 + 1);
    if (c2 == std::string_view::npos)
      return std::nullopt;
    auto h = base::StringToInt64(std::string(whole.substr(0, c1)));
    auto m =
        base::StringToInt64(std::string(whole.substr(c1 + 1, c2 - c1 - 1)));
    auto sec = base::StringToInt64(std::string(whole.substr(c2 + 1)));
    if (!h || !m || !sec)
      return std::nullopt;
    seconds = *h * 3600 + *m * 60 + *sec;
  }

  int64_t ns = seconds * kNsPerSec;
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

  // Optional leading pid (present with -f/-ff): "1234 14:32:01 ...".
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

  // Timestamp: a token containing ':' or, for -ttt, a run of digits
  // optionally followed by ".digits", ending at the next space.
  {
    size_t sp = rest.find(' ');
    if (sp == std::string_view::npos)
      return std::nullopt;
    std::string_view ts_tok = rest.substr(0, sp);
    auto ts = ParseTimeOfDay(ts_tok);
    if (!ts)
      return std::nullopt;
    out.tod_ns = *ts;
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

}  // namespace perfetto::trace_processor::strace_importer
