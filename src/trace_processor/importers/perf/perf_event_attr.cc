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

#include "src/trace_processor/importers/perf/perf_event_attr.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#include "src/trace_processor/importers/perf/perf_event.h"

namespace perfetto::trace_processor::perf_importer {

namespace {

constexpr auto kBytesPerField = 8;

size_t CountSetFlags(uint64_t sample_type) {
  return static_cast<size_t>(__builtin_popcountll(sample_type));
}

std::optional<size_t> TimeOffsetFromEndOfNonSampleRecord(
    const perf_event_attr& attr) {
  constexpr uint64_t kFlagsFromTimeToEnd =
      PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_STREAM_ID |
      PERF_SAMPLE_CPU | PERF_SAMPLE_IDENTIFIER;
  if (!attr.sample_id_all || !(attr.sample_type & PERF_SAMPLE_TIME)) {
    return std::nullopt;
  }
  return CountSetFlags(attr.sample_type & kFlagsFromTimeToEnd) * kBytesPerField;
}

std::optional<size_t> TimeOffsetFromStartOfSampleRecord(
    const perf_event_attr& attr) {
  constexpr uint64_t kFlagsFromStartToTime =
      PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_IP | PERF_SAMPLE_TID;
  if (!(attr.sample_type & PERF_SAMPLE_TIME)) {
    return std::nullopt;
  }
  return CountSetFlags(attr.sample_type & kFlagsFromStartToTime) *
         kBytesPerField;
}

std::optional<size_t> IdOffsetFromStartOfSampleRecord(
    const perf_event_attr& attr) {
  constexpr uint64_t kFlagsFromStartToId = PERF_SAMPLE_IDENTIFIER |
                                           PERF_SAMPLE_IP | PERF_SAMPLE_TID |
                                           PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR;

  if (attr.sample_type & PERF_SAMPLE_IDENTIFIER) {
    return 0;
  }

  if (attr.sample_type & PERF_SAMPLE_ID) {
    return CountSetFlags(attr.sample_type & kFlagsFromStartToId) *
           kBytesPerField;
  }
  return std::nullopt;
}

std::optional<size_t> IdOffsetFromEndOfNonSampleRecord(
    const perf_event_attr& attr) {
  constexpr uint64_t kFlagsFromIdToEnd =
      PERF_SAMPLE_ID | PERF_SAMPLE_STREAM_ID | PERF_SAMPLE_CPU |
      PERF_SAMPLE_IDENTIFIER;

  if (attr.sample_type & PERF_SAMPLE_IDENTIFIER) {
    return kBytesPerField;
  }

  if (attr.sample_type & PERF_SAMPLE_ID) {
    return CountSetFlags(attr.sample_type & kFlagsFromIdToEnd) * kBytesPerField;
  }

  return std::nullopt;
}
}  // namespace

PerfEventAttr::PerfEventAttr(perf_event_attr attr)
    : attr_(std::move(attr)),
      time_offset_from_start_(TimeOffsetFromStartOfSampleRecord(attr_)),
      time_offset_from_end_(TimeOffsetFromEndOfNonSampleRecord(attr_)),
      id_offset_from_start_(IdOffsetFromStartOfSampleRecord(attr_)),
      id_offset_from_end_(IdOffsetFromEndOfNonSampleRecord(attr_)) {}

}  // namespace perfetto::trace_processor::perf_importer
