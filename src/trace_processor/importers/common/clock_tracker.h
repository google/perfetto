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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_CLOCK_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_CLOCK_TRACKER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include "perfetto/base/status.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/clock_synchronizer.h"

namespace perfetto::trace_processor {

class ClockTrackerTest;
class TraceProcessorContext;

class ClockSynchronizerListenerImpl {
 private:
  TraceProcessorContext* context_;
  StringId source_clock_id_key_;
  StringId target_clock_id_key_;
  StringId source_timestamp_key_;
  StringId source_sequence_id_key_;
  StringId target_sequence_id_key_;

 public:
  explicit ClockSynchronizerListenerImpl(TraceProcessorContext* context);

  base::Status OnClockSyncCacheMiss();

  base::Status OnInvalidClockSnapshot();

  base::Status OnTraceTimeClockIdChanged(ClockSynchronizerBase::ClockId);

  base::Status OnSetTraceTimeClock(ClockSynchronizerBase::ClockId);

  void RecordConversionError(ClockSynchronizerBase::ErrorType,
                             ClockSynchronizerBase::ClockId source_clock_id,
                             ClockSynchronizerBase::ClockId target_clock_id,
                             int64_t source_timestamp,
                             std::optional<size_t>);

  bool IsLocalHost();
};

using ClockTracker = ClockSynchronizer<ClockSynchronizerListenerImpl>;

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_CLOCK_TRACKER_H_
