/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_redaction/redactor_clock_converter.h"

namespace perfetto::trace_redaction {

perfetto::trace_processor::ClockTracker::ClockId
RedactorClockConverter::GetPrimaryTraceClock() {
  return primary_trace_clock;
}

base::Status RedactorClockConverter::SetPrimaryTraceClock(
    trace_processor::ClockTracker::ClockId clock_id) {
  primary_trace_clock = clock_id;
  RETURN_IF_ERROR(clock_tracker.get()->SetTraceTimeClock(clock_id));
  return base::OkStatus();
}

void RedactorClockConverter::SetPerfTraceClock(
    trace_processor::ClockTracker::ClockId clock_id) {
  perf_clock = clock_id;
}

trace_processor::ClockTracker::ClockId
RedactorClockConverter::GetPerfTraceClock() {
  return perf_clock;
}

base::Status RedactorClockConverter::AddClockSnapshot(
    std::vector<trace_processor::ClockTracker::ClockTimestamp>&
        clock_snapshot) {
  base::StatusOr<uint32_t> snapshot_id =
      clock_tracker.get()->AddSnapshot(clock_snapshot);
  RETURN_IF_ERROR(snapshot_id.status());
  return base::OkStatus();
}

base::Status RedactorClockConverter::ConvertPerfToTrace(
    uint64_t perf_ts,
    uint64_t* out_ts) const {
  ASSIGN_OR_RETURN(int64_t trace_ts, clock_tracker.get()->ToTraceTime(
                                         static_cast<int64_t>(perf_clock),
                                         static_cast<int64_t>(perf_ts)));
  *out_ts = static_cast<uint64_t>(trace_ts);
  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
