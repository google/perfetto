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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ANDROID_BUGREPORT_ANDROID_LOG_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ANDROID_BUGREPORT_ANDROID_LOG_PARSER_H_

#include <stdint.h>

#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

struct AndroidLogEvent {
  int64_t ts;  // Nanoseconds since Epoch.
  uint32_t pid;
  uint32_t tid;
  uint32_t prio;  // Refer to enum ::protos::pbzero::AndroidLogPriority.
  StringId tag;
  StringId msg;

  // For std::sort().
  bool operator<(const AndroidLogEvent& o) const { return ts < o.ts; }

  // For gtest.
  bool operator==(const AndroidLogEvent& o) const {
    return std::tie(ts, pid, tid, prio, tag, msg) ==
           std::tie(o.ts, o.pid, o.tid, o.prio, o.tag, o.msg);
  }
};

// Parses log lines coming from persistent logcat (FS/data/misc/logd), interns
// string in the TP string pools and populates a vector of AndroidLogEvent
// structs. Does NOT insert log events into any table (for testing isolation),
// the caller is in charge to do that.
// It supports the following formats (auto-detected):
// 1) 12-31 23:59:00.123456 <pid> <tid> I tag: message
//    This is typically found in persistent logcat (FS/data/misc/logd/)
// 2) 06-24 15:57:11.346 <uid> <pid> <tid> D Tag: Message
//    This is typically found in the recent logcat dump in bugreport-xxx.txt
class AndroidLogParser {
 public:
  explicit AndroidLogParser(int year, TraceStorage* storage)
      : storage_(storage), year_(year) {}
  ~AndroidLogParser() = default;

  // Decodes logcat events for the input `lines` and appends them into
  // `log_events`. If `dedupe_idx` is != 0, it checks for duplicate entries
  // before inserting and skips the insertion if a dupe is found. Dupes are
  // searched in the first `dedupe_idx` entries of `log_events`. In practice
  // `dedupe_idx` is the log_events.size() for the last std::sort() call.
  // The de-duping logic truncates timestamps to millisecond resolution, to
  // handle the mismatching resolution of dumpstate (ms) vs persistent log (us).
  void ParseLogLines(std::vector<base::StringView> lines,
                     std::vector<AndroidLogEvent>* log_events,
                     size_t dedupe_idx = 0);

 private:
  TraceStorage* const storage_;
  int year_ = 0;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ANDROID_BUGREPORT_ANDROID_LOG_PARSER_H_
