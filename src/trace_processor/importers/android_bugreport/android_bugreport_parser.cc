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

#include "src/trace_processor/importers/android_bugreport/android_bugreport_parser.h"

#include <algorithm>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/android_bugreport/android_log_parser.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/zip_reader.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"

namespace perfetto {
namespace trace_processor {

AndroidBugreportParser::AndroidBugreportParser(TraceProcessorContext* ctx)
    : context_(ctx), zip_reader_(new util::ZipReader()) {}

AndroidBugreportParser::~AndroidBugreportParser() = default;

util::Status AndroidBugreportParser::Parse(TraceBlobView tbv) {
  if (!first_chunk_seen_) {
    first_chunk_seen_ = true;
    // All logs in Android bugreports use wall time (which creates problems
    // in case of early boot events before NTP kicks in, which get emitted as
    // 1970), but that is the state of affairs.
    context_->clock_tracker->SetTraceTimeClock(
        protos::pbzero::BUILTIN_CLOCK_REALTIME);
  }

  return zip_reader_->Parse(tbv.data(), tbv.size());
}

void AndroidBugreportParser::NotifyEndOfFile() {
  if (!DetectYear()) {
    context_->storage->IncrementStats(stats::android_br_parse_errors);
    return;
  }
  ParsePersistentLogcat();
}

void AndroidBugreportParser::ParsePersistentLogcat() {
  // 1. List logcat files in reverse timestmap order (old to most recent).
  // 2. Decode events from log lines into a vector. Dedupe and intern strings.
  // 3. Globally sort all extracted events.
  // 4. Insert into the android_logs table.

  AndroidLogParser log_parser(br_year_, context_->storage.get());

  // Sort files to ease the job of the subsequent line-based sort. Unfortunately
  // lines within each file are not 100% timestamp-ordered, due to things like
  // kernel messages where log time != event time.
  std::vector<std::pair<uint64_t, std::string>> log_paths;
  for (const util::ZipFile& zf : zip_reader_->files()) {
    if (base::StartsWith(zf.name(), "FS/data/misc/logd/logcat") &&
        !base::EndsWith(zf.name(), "logcat.id")) {
      log_paths.emplace_back(std::make_pair(zf.GetDatetime(), zf.name()));
    }
  }
  std::sort(log_paths.begin(), log_paths.end());

  // Push all events into the AndroidLogParser. It will take care of string
  // interning into the pool. Appends entries into `log_events`.
  std::vector<AndroidLogEvent> log_events;
  for (const auto& kv : log_paths) {
    util::ZipFile* zf = zip_reader_->Find(kv.second);
    zf->DecompressLines([&](const std::vector<base::StringView>& lines) {
      log_parser.ParseLogLines(lines, &log_events);
    });
  }

  // Sort the union of all log events parsed from all files in /data/misc/logd.
  std::sort(log_events.begin(), log_events.end());

  // Insert the globally sorted events into the android_logs table.
  for (const auto& e : log_events) {
    UniquePid utid = context_->process_tracker->UpdateThread(e.tid, e.pid);
    context_->storage->mutable_android_log_table()->Insert(
        {e.ts, utid, e.prio, e.tag, e.msg});
  }
}

// Populates the `year_` field from the bugreport-xxx.txt file name.
// This is because logcat events have only the month and day.
// This is obviously bugged for cases of bugreports collected across new year
// but we'll live with that.
bool AndroidBugreportParser::DetectYear() {
  const util::ZipFile* br_file = nullptr;
  for (const auto& zf : zip_reader_->files()) {
    if (base::StartsWith(zf.name(), "bugreport-") &&
        base::EndsWith(zf.name(), ".txt")) {
      br_file = &zf;
      break;
    }
  }

  if (!br_file) {
    PERFETTO_ELOG("Could not find bugreport-*.txt in the zip file");
    return false;
  }

  // Typical name: "bugreport-product-TP1A.220623.001-2022-06-24-16-24-37.txt".
  auto year_str = br_file->name().substr(
      br_file->name().size() - strlen("2022-12-31-23-59-00.txt"), 4);
  base::Optional<int32_t> year = base::StringToInt32(year_str);
  if (!year.has_value()) {
    PERFETTO_ELOG("Could not parse the year from %s", br_file->name().c_str());
    return false;
  }
  br_year_ = *year;
  return true;
}

}  // namespace trace_processor
}  // namespace perfetto
