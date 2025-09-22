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
#include "src/trace_processor/util/clock_synchronizer.h"

namespace perfetto::trace_redaction {

class RedactorClockSynchronizerListenerImpl {
 private:
  // Number of time that trace time has been updated.
  uint32_t trace_time_updates_;

 public:
  RedactorClockSynchronizerListenerImpl();

  base::Status OnClockSyncCacheMiss();

  base::Status OnInvalidClockSnapshot();

  base::Status OnTraceTimeClockIdChanged(
      perfetto::trace_processor::ClockSynchronizer<
          RedactorClockSynchronizerListenerImpl>::ClockId trace_time_clock_id);

  base::Status OnSetTraceTimeClock(
      perfetto::trace_processor::ClockSynchronizer<
          RedactorClockSynchronizerListenerImpl>::ClockId trace_time_clock_id);

  // Always returns true as redactor only supports local host clock conversion.
  bool IsLocalHost();
};

using RedactorClockSynchronizer = perfetto::trace_processor::ClockSynchronizer<
    RedactorClockSynchronizerListenerImpl>;

/**
 * This class handles conversions between different clocks for trace redactor.
 *
 * This class is a wrapper for trace_processor::ClockSynchronizer with the
 * addition that it caches clocks required for conversion for different data
 * sources and it is designed to be used by the trace redactor.
 *
 * Any trace packet intends to use the redactor ProcessThreadTimeline and whose
 * clock won't be the default trace time should use this class to convert
 * it to the default trace time which is used by ProcessThreadTimeline.
 */
class RedactorClockConverter {
 private:
  std::unique_ptr<RedactorClockSynchronizer> clock_synchronizer;
  RedactorClockSynchronizer::ClockId primary_trace_clock;
  RedactorClockSynchronizer::ClockId perf_clock;

 public:
  RedactorClockConverter() {
    // Set the default clocks for the trace
    primary_trace_clock = protos::pbzero::BuiltinClock::BUILTIN_CLOCK_BOOTTIME;
    perf_clock = protos::pbzero::BuiltinClock::BUILTIN_CLOCK_MONOTONIC_RAW;

    std::unique_ptr<RedactorClockSynchronizerListenerImpl> clock_listener =
        std::make_unique<RedactorClockSynchronizerListenerImpl>();
    clock_synchronizer =
        std::make_unique<RedactorClockSynchronizer>(std::move(clock_listener));
  }

  RedactorClockSynchronizer::ClockId GetPrimaryTraceClock();

  base::Status SetPrimaryTraceClock(
      RedactorClockSynchronizer::ClockId clock_id);

  void SetPerfTraceClock(RedactorClockSynchronizer::ClockId clock_id);

  RedactorClockSynchronizer::ClockId GetPerfTraceClock();

  base::Status AddClockSnapshot(
      std::vector<RedactorClockSynchronizer::ClockTimestamp>& clock_snapshot);

  base::Status ConvertPerfToTrace(uint64_t perf_ts, uint64_t* out_ts) const;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_REDACTOR_CLOCK_CONVERTER_H_
