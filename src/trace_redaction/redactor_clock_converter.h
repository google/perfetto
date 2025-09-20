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

#ifndef SRC_TRACE_REDACTION_REDACTOR_CLOCK_CONVERTER_H_
#define SRC_TRACE_REDACTION_REDACTOR_CLOCK_CONVERTER_H_

#include "perfetto/ext/base/status_macros.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "src/trace_processor/importers/common/clock_tracker.h"

namespace perfetto::trace_redaction {

/**
 * This class handles conversions between different clocks for trace redactor.
 *
 * This class is a wrapper for trace_processor::ClockTracker with the addition
 * that it keeps track of different clocks required for conversion for different
 * data sources.
 *
 * Any trace packet that won't use the default trace timestamp and intends to
 * use the redactor ProcessThreadTimeline should use this class to convert
 * between different clocks.
 */
class RedactorClockConverter {
 private:
  std::unique_ptr<trace_processor::ClockTracker> clock_tracker;
  trace_processor::ClockTracker::ClockId primary_trace_clock;
  trace_processor::ClockTracker::ClockId perf_clock;

 public:
  RedactorClockConverter() {
    // Set the default clocks for the trace
    primary_trace_clock = protos::pbzero::BuiltinClock::BUILTIN_CLOCK_BOOTTIME;
    perf_clock = protos::pbzero::BuiltinClock::BUILTIN_CLOCK_MONOTONIC_RAW;

    clock_tracker = std::make_unique<trace_processor::ClockTracker>(nullptr);
  }

  trace_processor::ClockTracker::ClockId GetPrimaryTraceClock();

  base::Status SetPrimaryTraceClock(
      trace_processor::ClockTracker::ClockId clock_id);

  void SetPerfTraceClock(trace_processor::ClockTracker::ClockId clock_id);

  trace_processor::ClockTracker::ClockId GetPerfTraceClock();

  base::Status AddClockSnapshot(
      std::vector<trace_processor::ClockTracker::ClockTimestamp>&
          clock_snapshot);

  base::Status ConvertPerfToTrace(uint64_t perf_ts, uint64_t* out_ts) const;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_REDACTOR_CLOCK_CONVERTER_H_
