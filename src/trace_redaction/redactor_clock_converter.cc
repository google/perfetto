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

using ClockId = RedactorClockSynchronizer::ClockId;

RedactorClockSynchronizerListenerImpl::RedactorClockSynchronizerListenerImpl()
    : trace_time_updates_(0) {}

base::Status RedactorClockSynchronizerListenerImpl::OnClockSyncCacheMiss() {
  return base::OkStatus();
}

base::Status RedactorClockSynchronizerListenerImpl::OnInvalidClockSnapshot() {
  PERFETTO_ELOG("Invalid clocks snapshot found during redaction");
  return base::ErrStatus("Invalid clocks snapshot found during redaction");
}

base::Status RedactorClockSynchronizerListenerImpl::OnTraceTimeClockIdChanged(
    ClockId trace_time_clock_id [[maybe_unused]]) {
  ++trace_time_updates_;
  if (PERFETTO_UNLIKELY(trace_time_updates_ > 1)) {
    // We expect the trace time to remain constant for a trace.
    PERFETTO_ELOG(
        "Redactor clock conversion trace time unexpectedly changed %d times",
        trace_time_updates_);
    return base::ErrStatus(
        "Redactor clock conversion trace time unexpectedly changed %d times",
        trace_time_updates_);
  }
  return base::OkStatus();
}

base::Status RedactorClockSynchronizerListenerImpl::OnSetTraceTimeClock(
    ClockId trace_time_clock_id [[maybe_unused]]) {
  return base::OkStatus();
}

bool RedactorClockSynchronizerListenerImpl::IsLocalHost() {
  // Redactor does not support multi-machine clock conversion
  return true;
}

ClockId RedactorClockConverter::GetPrimaryTraceClock() {
  return primary_trace_clock;
}

base::Status RedactorClockConverter::SetPrimaryTraceClock(ClockId clock_id) {
  primary_trace_clock = clock_id;
  RETURN_IF_ERROR(clock_synchronizer.get()->SetTraceTimeClock(clock_id));
  return base::OkStatus();
}

void RedactorClockConverter::SetPerfTraceClock(ClockId clock_id) {
  perf_clock = clock_id;
}

ClockId RedactorClockConverter::GetPerfTraceClock() {
  return perf_clock;
}

base::Status RedactorClockConverter::AddClockSnapshot(
    std::vector<RedactorClockSynchronizer::ClockTimestamp>& clock_snapshot) {
  base::StatusOr<uint32_t> snapshot_id =
      clock_synchronizer.get()->AddSnapshot(clock_snapshot);
  RETURN_IF_ERROR(snapshot_id.status());
  return base::OkStatus();
}

base::Status RedactorClockConverter::ConvertPerfToTrace(
    uint64_t perf_ts,
    uint64_t* out_ts) const {
  ASSIGN_OR_RETURN(int64_t trace_ts, clock_synchronizer.get()->ToTraceTime(
                                         static_cast<int64_t>(perf_clock),
                                         static_cast<int64_t>(perf_ts)));
  *out_ts = static_cast<uint64_t>(trace_ts);
  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
