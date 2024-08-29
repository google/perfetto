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

#include "src/trace_processor/importers/android_bugreport/android_dumpstate_reader.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/android_bugreport/android_log_reader.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {

AndroidDumpstateReader::AndroidDumpstateReader(
    TraceProcessorContext* context,
    int32_t year,
    std::vector<TimestampedAndroidLogEvent> logcat_events)
    : context_(context), log_reader_(context, year, std::move(logcat_events)) {}

AndroidDumpstateReader::~AndroidDumpstateReader() = default;

base::Status AndroidDumpstateReader::ParseLine(base::StringView line) {
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
  static constexpr size_t npos = base::StringView::npos;
  if (line.StartsWith("------ ") && line.EndsWith(" ------")) {
    // These lines mark the beginning and end of dumpstate sections:
    // ------ DUMPSYS CRITICAL (/system/bin/dumpsys) ------
    // ------ 0.356s was the duration of 'DUMPSYS CRITICAL' ------
    base::StringView section = line.substr(7);
    section = section.substr(0, section.size() - 7);
    bool end_marker = section.find("was the duration of") != npos;
    current_service_id_ = StringId::Null();
    if (end_marker) {
      current_section_id_ = StringId::Null();
    } else {
      current_section_id_ = context_->storage->InternString(section);
      current_section_ = Section::kOther;
      if (section.StartsWith("DUMPSYS")) {
        current_section_ = Section::kDumpsys;
      } else if (section.StartsWith("SYSTEM LOG") ||
                 section.StartsWith("EVENT LOG") ||
                 section.StartsWith("RADIO LOG")) {
        // KERNEL LOG is deliberately omitted because SYSTEM LOG is a
        // superset. KERNEL LOG contains all dupes.
        current_section_ = Section::kLog;
      } else if (section.StartsWith("BLOCK STAT")) {
        // Coalesce all the block stats into one section. Otherwise they
        // pollute the table with one section per block device.
        current_section_id_ = context_->storage->InternString("BLOCK STAT");
      }
    }
    return base::OkStatus();
  }
  // Skip end marker lines for dumpsys sections.
  if (current_section_ == Section::kDumpsys && line.StartsWith("--------- ") &&
      line.find("was the duration of dumpsys") != npos) {
    current_service_id_ = StringId::Null();
    return base::OkStatus();
  }
  if (current_section_ == Section::kDumpsys && current_service_id_.is_null() &&
      line.StartsWith("----------------------------------------------")) {
    return base::OkStatus();
  }
  if (current_section_ == Section::kDumpsys &&
      line.StartsWith("DUMP OF SERVICE")) {
    // DUMP OF SERVICE [CRITICAL|HIGH] ServiceName:
    base::StringView svc = line.substr(line.rfind(' ') + 1);
    svc = svc.substr(0, svc.size() - 1);
    current_service_id_ = context_->storage->InternString(svc);
  } else if (current_section_ == Section::kLog) {
    RETURN_IF_ERROR(log_reader_.ParseLine(line));
  }

  // Append the line to the android_dumpstate table.
  context_->storage->mutable_android_dumpstate_table()->Insert(
      {current_section_id_, current_service_id_,
       context_->storage->InternString(line)});

  return base::OkStatus();
}

void AndroidDumpstateReader::EndOfStream(base::StringView) {}

}  // namespace perfetto::trace_processor
