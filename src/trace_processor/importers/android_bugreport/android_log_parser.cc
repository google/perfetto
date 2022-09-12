/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/importers/android_bugreport/android_log_parser.h"

#include <string.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/common/android_log_constants.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {

// Reads a base-10 number and advances the passed StringView beyond the *last*
// instance of `sep`. Example:
// Input:  it="1234   bar".
// Output: it="bar", ret=1234.
//
// `decimal_scale` is used to parse decimals and defines the output resolution.
// E.g. input="1",    decimal_scale=1000 -> res=100
//      input="12",   decimal_scale=1000 -> res=120
//      input="123",  decimal_scale=1000 -> res=123
//      input="1234", decimal_scale=1000 -> res=123
//      input="1234", decimal_scale=1000000 -> res=123400
base::Optional<int> ReadNumAndAdvance(base::StringView* it,
                                      char sep,
                                      int decimal_scale = 0) {
  int num = 0;
  bool sep_found = false;
  size_t next_it = 0;
  bool invalid_chars_found = false;
  for (size_t i = 0; i < it->size(); i++) {
    char c = it->at(i);
    if (c == sep) {
      next_it = i + 1;
      sep_found = true;
      continue;
    }
    if (sep_found)
      break;
    if (c >= '0' && c <= '9') {
      int digit = static_cast<int>(c - '0');
      if (!decimal_scale) {
        num = num * 10 + digit;
      } else {
        decimal_scale /= 10;
        num += digit * decimal_scale;
      }
      continue;
    }
    // We found something that is not a digit. Keep looking for the next `sep`
    // but flag the current token as invalid.
    invalid_chars_found = true;
  }
  if (!sep_found)
    return base::nullopt;
  // If we find non-digit characters, we want to still skip the token but return
  // nullopt. The parser below relies on token skipping to deal with cases where
  // the uid (which we don't care about) is literal ("root" rather than 0).
  *it = it->substr(next_it);
  if (invalid_chars_found)
    return base::nullopt;
  return num;
}

enum class LogcatFormat {
  kUnknown = 0,

  // 01-02 03:04:05.678901 1000 2000 V Tag: Message
  kPersistentLog,

  // 06-24 15:57:11.346  1000  1493  1918 D Tag: Message
  // or also
  // 07-28 14:25:22.181  root     0     0 I Tag : Message
  kBugreport
};

LogcatFormat DetectFormat(base::StringView line) {
  auto p = base::SplitString(line.ToStdString(), " ");
  if (p.size() < 5)
    return LogcatFormat::kUnknown;

  if (p[0].size() != 5 || p[0][2] != '-')
    return LogcatFormat::kUnknown;

  if (p[1].size() < 10 || p[1][2] != ':' || p[1][5] != ':' || p[1][8] != '.')
    return LogcatFormat::kUnknown;

  if (p[4].size() == 1 && p[4][0] >= 'A' && p[4][0] <= 'Z')
    return LogcatFormat::kPersistentLog;

  if (p[5].size() == 1 && p[5][0] >= 'A' && p[5][0] <= 'Z')
    return LogcatFormat::kBugreport;

  return LogcatFormat::kUnknown;
}

}  // namespace

// Parses a bunch of logcat lines and appends broken down events into
// `log_events`.
void AndroidLogParser::ParseLogLines(std::vector<base::StringView> lines,
                                     std::vector<AndroidLogEvent>* log_events,
                                     size_t dedupe_idx) {
  int parse_failures = 0;
  LogcatFormat fmt = LogcatFormat::kUnknown;
  for (auto line : lines) {
    if (line.size() < 30 ||
        (line.at(0) == '-' && line.at(1) == '-' && line.at(2) == '-')) {
      // These are markers like "--------- switch to radio" which we ignore.
      // The smallest valid logcat line has around 30 chars, as follows:
      // "06-24 23:10:00.123  1 1 D : ..."
      continue;
    }
    if (fmt == LogcatFormat::kUnknown) {
      fmt = DetectFormat(line);
      if (fmt == LogcatFormat::kUnknown) {
        PERFETTO_DLOG("Could not detect logcat format for: |%s|",
                      line.ToStdString().c_str());
        storage_->IncrementStats(stats::android_log_format_invalid);
        return;
      }
    }

    base::StringView it = line;
    // 06-24 16:24:23.441532 23153 23153 I wm_on_stop_called: message ...
    // 07-28 14:25:13.506  root     0     0 I x86/fpu : Supporting XSAVE feature
    // 0x002: 'SSE registers'
    base::Optional<int> month = ReadNumAndAdvance(&it, '-');
    base::Optional<int> day = ReadNumAndAdvance(&it, ' ');
    base::Optional<int> hour = ReadNumAndAdvance(&it, ':');
    base::Optional<int> minute = ReadNumAndAdvance(&it, ':');
    base::Optional<int> sec = ReadNumAndAdvance(&it, '.');
    base::Optional<int> ns = ReadNumAndAdvance(&it, ' ', 1000 * 1000 * 1000);

    if (fmt == LogcatFormat::kBugreport)
      ReadNumAndAdvance(&it, ' ');  // Skip the UID column.

    base::Optional<int> pid = ReadNumAndAdvance(&it, ' ');
    base::Optional<int> tid = ReadNumAndAdvance(&it, ' ');

    if (!month || !day || !hour || !minute || !sec || !ns || !pid || !tid) {
      ++parse_failures;
      continue;
    }

    if (it.size() < 4 || it.at(1) != ' ') {
      ++parse_failures;
      continue;
    }

    char prio_str = it.at(0);
    int prio = protos::pbzero::AndroidLogPriority::PRIO_UNSPECIFIED;
    if ('V' == prio_str) {
      prio = protos::pbzero::AndroidLogPriority::PRIO_VERBOSE;
    } else if ('D' == prio_str) {
      prio = protos::pbzero::AndroidLogPriority::PRIO_DEBUG;
    } else if ('I' == prio_str) {
      prio = protos::pbzero::AndroidLogPriority::PRIO_INFO;
    } else if ('W' == prio_str) {
      prio = protos::pbzero::AndroidLogPriority::PRIO_WARN;
    } else if ('E' == prio_str) {
      prio = protos::pbzero::AndroidLogPriority::PRIO_ERROR;
    } else if ('F' == prio_str) {
      prio = protos::pbzero::AndroidLogPriority::PRIO_FATAL;
    }

    it = it.substr(2);

    // Find the ': ' that defines the boundary between the tag and message.
    // We can't just look for ':' because various HALs emit tags with a ':'.
    base::StringView cat;
    for (size_t i = 0; i < it.size() - 1; ++i) {
      if (it.at(i) == ':' && it.at(i + 1) == ' ') {
        cat = it.substr(0, i);
        it = it.substr(i + 2);
        break;
      }
    }
    // Trim trailing spaces, happens in kernel events (e.g. "init   :").
    while (!cat.empty() && cat.at(cat.size() - 1) == ' ')
      cat = cat.substr(0, cat.size() - 1);

    base::StringView msg = it;  // The rest is the log message.

    int64_t secs = base::MkTime(year_, *month, *day, *hour, *minute, *sec);
    int64_t ts = secs * 1000000000ll + *ns;

    AndroidLogEvent evt{ts,
                        static_cast<uint32_t>(*pid),
                        static_cast<uint32_t>(*tid),
                        static_cast<uint32_t>(prio),
                        storage_->InternString(cat),
                        storage_->InternString(msg)};

    if (dedupe_idx > 0) {
      // Search for dupes before inserting.
      // Events in the [0, dedupe_idx] range are sorted by timestamp with ns
      // resolution. Here we search for dupes within the same millisecond of
      // the event we are trying to insert. The /1000000*1000000 is to deal with
      // the fact that events coming from the persistent log have us resolution,
      // while events from dumpstate (which are often dupes of persistent ones)
      // have only ms resolution. Here we consider an event a dupe if it has
      // the same ms-truncated solution, same pid, tid and message.
      AndroidLogEvent etrunc = evt;
      etrunc.ts = etrunc.ts / 1000000 * 1000000;
      auto begin = log_events->begin();
      auto end = log_events->begin() + static_cast<ssize_t>(dedupe_idx);
      bool dupe_found = false;
      for (auto eit = std::lower_bound(begin, end, etrunc); eit < end; ++eit) {
        if (eit->ts / 1000000 * 1000000 != etrunc.ts)
          break;
        if (eit->msg == evt.msg && eit->tag == evt.tag && eit->tid == evt.tid &&
            eit->pid == evt.pid) {
          dupe_found = true;
          break;
        }
      }
      if (dupe_found) {
        continue;  // Skip the current line.
      }
    }  // if (dedupe_idx)

    log_events->emplace_back(std::move(evt));
  }  //  for (line : lines)
  storage_->IncrementStats(stats::android_log_num_failed, parse_failures);
}

}  // namespace trace_processor
}  // namespace perfetto
