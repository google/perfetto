/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/android_bugreport/android_battery_stats_reader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/android_bugreport/android_battery_stats_history_string_tracker.h"
#include "src/trace_processor/importers/android_bugreport/android_dumpstate_event.h"
#include "src/trace_processor/importers/common/clock_converter.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {

namespace {

base::StatusOr<uint64_t> StringToStatusOrUInt64(base::StringView str) {
  std::optional<uint64_t> possible_result =
      base::StringToUInt64(str.ToStdString());
  if (!possible_result.has_value()) {
    return base::ErrStatus("Failed to convert string to uint64_t");
  }
  return possible_result.value();
}
}  // namespace

AndroidBatteryStatsReader::AndroidBatteryStatsReader(
    TraceProcessorContext* context)
    : context_(context) {}

AndroidBatteryStatsReader::~AndroidBatteryStatsReader() = default;

util::Status AndroidBatteryStatsReader::ParseLine(base::StringView line) {
  // TODO: migrate to future StringViewSplitter when availabile.
  base::StringSplitter splitter(line.ToStdString(), ',');

  // consume the legacy version number which we expect to be at the start of
  // every line.
  if ((splitter.Next() ? std::string(splitter.cur_token()) : "") != "9") {
    return base::ErrStatus("Unexpected start of battery stats checkin line");
  }

  const base::StringView possible_event_type =
      splitter.Next() ? splitter.cur_token() : "";
  if (possible_event_type == "hsp") {
    ASSIGN_OR_RETURN(
        uint64_t index,
        StringToStatusOrUInt64(splitter.Next() ? splitter.cur_token() : ""));
    const std::optional<int32_t> possible_uid =
        base::StringToInt32(splitter.Next() ? splitter.cur_token() : "");
    const base::StringView hsp_string =
        splitter.Next() ? splitter.cur_token() : "";
    AndroidBatteryStatsHistoryStringTracker::GetOrCreate(context_)
        ->SetStringPoolItem(index, possible_uid.value(),
                            hsp_string.ToStdString());
  } else if (possible_event_type == "h") {
    const base::StringView time_adjustment_marker = ":TIME:";
    const base::StringView possible_timestamp =
        splitter.Next() ? splitter.cur_token() : "";
    size_t time_marker_index = possible_timestamp.find(time_adjustment_marker);
    if (time_marker_index != base::StringView::npos) {
      // Special case timestamp adjustment event.
      ASSIGN_OR_RETURN(current_timestamp_ms_,
                       StringToStatusOrUInt64(possible_timestamp.substr(
                           time_marker_index + time_adjustment_marker.size())));
      return base::OkStatus();
    } else if (possible_timestamp.find(":START") != base::StringView::npos) {
      // Ignore line
      return base::OkStatus();
    } else if (possible_timestamp.find(":SHUTDOWN") != base::StringView::npos) {
      // Ignore line
      return base::OkStatus();
    } else {
      ASSIGN_OR_RETURN(uint64_t parsed_timestamp_delta,
                       StringToStatusOrUInt64(possible_timestamp));
      current_timestamp_ms_ += parsed_timestamp_delta;
      for (base::StringView item = splitter.Next() ? splitter.cur_token() : "";
           !item.empty(); item = splitter.Next() ? splitter.cur_token() : "") {
        RETURN_IF_ERROR(ProcessBatteryStatsHistoryEvent(item));
      }
    }
  } else {
    // TODO Implement UID parsing and other kinds of events.
  }

  return base::OkStatus();
}

util::Status AndroidBatteryStatsReader::ProcessBatteryStatsHistoryEvent(
    base::StringView raw_event) {
  AndroidDumpstateEvent event{
      AndroidDumpstateEvent::EventType::kBatteryStatsHistoryEvent,
      raw_event.ToStdString()};
  return SendToSorter(std::chrono::milliseconds(current_timestamp_ms_), event);
}

util::Status AndroidBatteryStatsReader::SendToSorter(
    std::chrono::nanoseconds event_ts,
    AndroidDumpstateEvent event) {
  ASSIGN_OR_RETURN(
      int64_t trace_ts,
      context_->clock_tracker->ToTraceTime(
          protos::pbzero::ClockSnapshot::Clock::REALTIME, event_ts.count()));
  context_->sorter->PushAndroidDumpstateEvent(trace_ts, std::move(event));
  return base::OkStatus();
}

void AndroidBatteryStatsReader::EndOfStream(base::StringView) {}

}  // namespace perfetto::trace_processor
