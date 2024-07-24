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

#include "src/trace_processor/importers/android_bugreport/android_bugreport_reader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "src/trace_processor/importers/android_bugreport/android_dumpstate_reader.h"
#include "src/trace_processor/importers/android_bugreport/android_log_reader.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/trace_processor/util/trace_type.h"
#include "src/trace_processor/util/zip_reader.h"

namespace perfetto::trace_processor {
namespace {
const util::ZipFile* FindBugReportFile(
    const std::vector<util::ZipFile>& zip_file_entries) {
  for (const auto& zf : zip_file_entries) {
    if (base::StartsWith(zf.name(), "bugreport-") &&
        base::EndsWith(zf.name(), ".txt")) {
      return &zf;
    }
  }
  return nullptr;
}

std::optional<int32_t> ExtractYearFromBugReportFilename(
    const std::string& filename) {
  // Typical name: "bugreport-product-TP1A.220623.001-2022-06-24-16-24-37.txt".
  auto year_str =
      filename.substr(filename.size() - strlen("2022-12-31-23-59-00.txt"), 4);
  return base::StringToInt32(year_str);
}

}  // namespace

// static
bool AndroidBugreportReader::IsAndroidBugReport(
    const std::vector<util::ZipFile>& zip_file_entries) {
  if (const util::ZipFile* file = FindBugReportFile(zip_file_entries);
      file != nullptr) {
    return ExtractYearFromBugReportFilename(file->name()).has_value();
  }

  return false;
}

// static
util::Status AndroidBugreportReader::Parse(
    TraceProcessorContext* context,
    std::vector<util::ZipFile> zip_file_entries) {
  if (!IsAndroidBugReport(zip_file_entries)) {
    return base::ErrStatus("Not a bug report");
  }
  return AndroidBugreportReader(context, std::move(zip_file_entries))
      .ParseImpl();
}

AndroidBugreportReader::AndroidBugreportReader(
    TraceProcessorContext* context,
    std::vector<util::ZipFile> zip_file_entries)
    : context_(context), zip_file_entries_(std::move(zip_file_entries)) {}

AndroidBugreportReader::~AndroidBugreportReader() = default;

util::Status AndroidBugreportReader::ParseImpl() {
  // All logs in Android bugreports use wall time (which creates problems
  // in case of early boot events before NTP kicks in, which get emitted as
  // 1970), but that is the state of affairs.
  context_->clock_tracker->SetTraceTimeClock(
      protos::pbzero::BUILTIN_CLOCK_REALTIME);
  if (!DetectYearAndBrFilename()) {
    context_->storage->IncrementStats(stats::android_br_parse_errors);
    return base::ErrStatus("Zip file does not contain bugreport file.");
  }

  ASSIGN_OR_RETURN(std::vector<TimestampedAndroidLogEvent> logcat_events,
                   ParsePersistentLogcat());
  return ParseDumpstateTxt(std::move(logcat_events));
}

base::Status AndroidBugreportReader::ParseDumpstateTxt(
    std::vector<TimestampedAndroidLogEvent> logcat_events) {
  PERFETTO_CHECK(dumpstate_file_);
  ScopedActiveTraceFile trace_file = context_->trace_file_tracker->StartNewFile(
      dumpstate_file_->name(), kAndroidDumpstateTraceType,
      dumpstate_file_->uncompressed_size());
  AndroidDumpstateReader reader(context_, br_year_, std::move(logcat_events));
  return dumpstate_file_->DecompressLines(
      [&](const std::vector<base::StringView>& lines) {
        for (const base::StringView& line : lines) {
          reader.ParseLine(line);
        }
      });
}

base::StatusOr<std::vector<TimestampedAndroidLogEvent>>
AndroidBugreportReader::ParsePersistentLogcat() {
  BufferingAndroidLogReader log_reader(context_, br_year_);

  // Sort files to ease the job of the subsequent line-based sort. Unfortunately
  // lines within each file are not 100% timestamp-ordered, due to things like
  // kernel messages where log time != event time.
  std::vector<std::pair<uint64_t, const util::ZipFile*>> log_files;
  for (const util::ZipFile& zf : zip_file_entries_) {
    if (base::StartsWith(zf.name(), "FS/data/misc/logd/logcat") &&
        !base::EndsWith(zf.name(), "logcat.id")) {
      log_files.push_back(std::make_pair(zf.GetDatetime(), &zf));
    }
  }

  std::sort(log_files.begin(), log_files.end());

  // Push all events into the AndroidLogParser. It will take care of string
  // interning into the pool. Appends entries into `log_events`.
  for (const auto& log_file : log_files) {
    ScopedActiveTraceFile trace_file =
        context_->trace_file_tracker->StartNewFile(
            log_file.second->name(), kAndroidLogcatTraceType,
            log_file.second->uncompressed_size());
    RETURN_IF_ERROR(log_file.second->DecompressLines(
        [&](const std::vector<base::StringView>& lines) {
          for (const auto& line : lines) {
            log_reader.ParseLine(line);
          }
        }));
  }

  return std::move(log_reader).ConsumeBufferedEvents();
}

// Populates the `year_` field from the bugreport-xxx.txt file name.
// This is because logcat events have only the month and day.
// This is obviously bugged for cases of bugreports collected across new year
// but we'll live with that.
bool AndroidBugreportReader::DetectYearAndBrFilename() {
  const util::ZipFile* br_file = FindBugReportFile(zip_file_entries_);
  if (!br_file) {
    PERFETTO_ELOG("Could not find bugreport-*.txt in the zip file");
    return false;
  }

  std::optional<int32_t> year =
      ExtractYearFromBugReportFilename(br_file->name());
  if (!year.has_value()) {
    PERFETTO_ELOG("Could not parse the year from %s", br_file->name().c_str());
    return false;
  }
  br_year_ = *year;
  dumpstate_file_ = br_file;
  return true;
}

}  // namespace perfetto::trace_processor
