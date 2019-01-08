/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_CLOCK_TRACKER_H_
#define SRC_TRACE_PROCESSOR_CLOCK_TRACKER_H_

#include <stdint.h>

#include <array>
#include <vector>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

enum ClockDomain : uint32_t {
  kBootTime,   // Monotonic, counts also time in suspend mode.
  kMonotonic,  // Monotonic, doesn't advance when the device is suspended.
  kRealTime,   // Real time clock, can move backward (e.g. NTP adjustements).
  kNumClockDomains
};

class ClockTracker {
 public:
  explicit ClockTracker(TraceProcessorContext*);
  virtual ~ClockTracker();

  // Push a snapshot that tells what is the corresponding trace time for the
  // given |clock_time_ns| in the given clock domain. This is typically called
  // by the code that reads the ClockSnapshot packet.
  void SyncClocks(ClockDomain, int64_t clock_time_ns, int64_t trace_time_ns);

  // Converts the passed time in the given clock domain to the global trace
  // time (CLOCK_BOOTTIME for Android traces).
  int64_t ToTraceTime(ClockDomain, int64_t clock_time_ns);

  int64_t GetFirstTimestamp(ClockDomain domain) const {
    PERFETTO_DCHECK(!clocks_[domain].empty());
    return clocks_[domain].front().clock_time_ns;
  }

 private:
  ClockTracker(const ClockTracker&) = delete;
  ClockTracker& operator=(const ClockTracker&) = delete;

  struct ClockSnapshot {
    int64_t clock_time_ns;
    int64_t trace_time_ns;
  };

  TraceProcessorContext* const context_;
  using ClockSnapshotVector = std::vector<ClockSnapshot>;
  std::array<ClockSnapshotVector, kNumClockDomains> clocks_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_CLOCK_TRACKER_H_
