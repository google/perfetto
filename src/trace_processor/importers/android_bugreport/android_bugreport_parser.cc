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
  if (!DetectYearAndBrFilename()) {
    context_->storage->IncrementStats(stats::android_br_parse_errors);
    return;
  }

  ParsePersistentLogcat();
  ParseDumpstateTxt();
  SortAndStoreLogcat();
}

void AndroidBugreportParser::ParseDumpstateTxt() {
  // Dumpstate is organized in a two level hierarchy, beautifully flattened into
  // one text file with load bearing ----- markers:
  // 1. Various dumpstate sections, examples:
  // ```
  //   ------ DUMPSYS CRITICAL (/system/bin/dumpsys) ------
  //   ...
  //   ------ SYSTEM LOG (logcat -v threadtime -v printable -v uid) ------
  //   ...
  //   ------ IPTABLES (iptables -L -nvx) ------
  //   ...
  //   ------ DUMPSYS HIGH (/system/bin/dumpsys) ------
  //   ...
  //   ------ DUMPSYS (/system/bin/dumpsys) ------
  // ```
  //
  // 2. Within the "------ DUMPSYS" section (note dumpsys != dumpstate), there
  //    are multiple services. Note that there are at least 3 DUMPSYS sections
  //    (CRITICAL, HIGH and default), with multiple services in each:
  // ```
  //    ------ DUMPSYS (/system/bin/dumpsys) ------
  // DUMP OF SERVICE activity:
  // ...
  // ---------------------------------------------------------------------------
  // DUMP OF SERVICE input_method:
  // ...
  // ---------------------------------------------------------------------------
  // ```
  // Here we put each line in a dedicated table, android_dumpstate, keeping
  // track of the dumpstate `section` and dumpsys `service`.
  AndroidLogParser log_parser(br_year_, context_->storage.get());
  util::ZipFile* zf = zip_reader_->Find(dumpstate_fname_);
  StringId section_id = StringId::Null();  // The current dumpstate section.
  StringId service_id = StringId::Null();  // The current dumpsys service.
  static constexpr size_t npos = base::StringView::npos;
  enum { OTHER = 0, DUMPSYS, LOG } cur_sect = OTHER;
  zf->DecompressLines([&](const std::vector<base::StringView>& lines) {
    // Optimization for ParseLogLines() below. Avoids ctor/dtor-ing a new vector
    // on every line.
    std::vector<base::StringView> log_line(1);
    for (const base::StringView& line : lines) {
      if (line.StartsWith("------ ") && line.EndsWith(" ------")) {
        // These lines mark the beginning and end of dumpstate sections:
        // ------ DUMPSYS CRITICAL (/system/bin/dumpsys) ------
        // ------ 0.356s was the duration of 'DUMPSYS CRITICAL' ------
        base::StringView section = line.substr(7);
        section = section.substr(0, section.size() - 7);
        bool end_marker = section.find("was the duration of") != npos;
        service_id = StringId::Null();
        if (end_marker) {
          section_id = StringId::Null();
        } else {
          section_id = context_->storage->InternString(section);
          cur_sect = OTHER;
          if (section.StartsWith("DUMPSYS")) {
            cur_sect = DUMPSYS;
          } else if (section.StartsWith("SYSTEM LOG") ||
                     section.StartsWith("EVENT LOG") ||
                     section.StartsWith("RADIO LOG")) {
            // KERNEL LOG is deliberately omitted because SYSTEM LOG is a
            // superset. KERNEL LOG contains all dupes.
            cur_sect = LOG;
          } else if (section.StartsWith("BLOCK STAT")) {
            // Coalesce all the block stats into one section. Otherwise they
            // pollute the table with one section per block device.
            section_id = context_->storage->InternString("BLOCK STAT");
          }
        }
        continue;
      }
      // Skip end marker lines for dumpsys sections.
      if (cur_sect == DUMPSYS && line.StartsWith("--------- ") &&
          line.find("was the duration of dumpsys") != npos) {
        service_id = StringId::Null();
        continue;
      }
      if (cur_sect == DUMPSYS && service_id.is_null() &&
          line.StartsWith("----------------------------------------------")) {
        continue;
      }
      if (cur_sect == DUMPSYS && line.StartsWith("DUMP OF SERVICE")) {
        // DUMP OF SERVICE [CRITICAL|HIGH] ServiceName:
        base::StringView svc = line.substr(line.rfind(' ') + 1);
        svc = svc.substr(0, svc.size() - 1);
        service_id = context_->storage->InternString(svc);
      } else if (cur_sect == LOG) {
        // Parse the non-persistent logcat and append to `log_events_`, together
        // with the persistent one previously parsed by ParsePersistentLogcat().
        // Skips entries that are already seen in the persistent logcat,
        // handling us vs ms truncation.
        PERFETTO_DCHECK(log_line.size() == 1);
        log_line[0] = line;
        log_parser.ParseLogLines(log_line, &log_events_,
                                 log_events_last_sorted_idx_);
      }

      if (build_fpr_.empty() && line.StartsWith("Build fingerprint:")) {
        build_fpr_ = line.substr(20, line.size() - 20).ToStdString();
      }

      // Append the line to the android_dumpstate table.
      context_->storage->mutable_android_dumpstate_table()->Insert(
          {section_id, service_id, context_->storage->InternString(line)});
    }
  });
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
  for (const auto& kv : log_paths) {
    util::ZipFile* zf = zip_reader_->Find(kv.second);
    zf->DecompressLines([&](const std::vector<base::StringView>& lines) {
      log_parser.ParseLogLines(lines, &log_events_);
    });
  }

  // Do an initial sorting pass. This is not the final sorting because we
  // haven't ingested the latest logs from dumpstate yet. But we need this sort
  // to be able to de-dupe the same lines showing both in dumpstate and in the
  // persistent log.
  SortLogEvents();
}

void AndroidBugreportParser::SortAndStoreLogcat() {
  // Sort the union of all log events parsed from both /data/misc/logd
  // (persistent logcat on disk) and the dumpstate file (last in-memory logcat).
  // Before the std::stable_sort, entries in `log_events_` are already "mostly"
  // sorted, because we processed files in order (see notes above about kernel
  // logs on why we need a final sort here).
  // We need stable-sort to preserve FIFO-ness of events emitted at the same
  // time, logcat is not granular enough (us for persistent, ms for dumpstate).
  SortLogEvents();

  // Insert the globally sorted events into the android_logs table.
  for (const auto& e : log_events_) {
    UniquePid utid = context_->process_tracker->UpdateThread(e.tid, e.pid);
    context_->storage->mutable_android_log_table()->Insert(
        {e.ts, utid, e.prio, e.tag, e.msg});
  }
}

// Populates the `year_` field from the bugreport-xxx.txt file name.
// This is because logcat events have only the month and day.
// This is obviously bugged for cases of bugreports collected across new year
// but we'll live with that.
bool AndroidBugreportParser::DetectYearAndBrFilename() {
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
  dumpstate_fname_ = br_file->name();
  return true;
}

void AndroidBugreportParser::SortLogEvents() {
  std::stable_sort(log_events_.begin(), log_events_.end());
  log_events_last_sorted_idx_ = log_events_.size();
}

}  // namespace trace_processor
}  // namespace perfetto
