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

#ifndef SRC_TRACE_PROCESSOR_TRACE_SORTER_INTERNAL_H_
#define SRC_TRACE_PROCESSOR_TRACE_SORTER_INTERNAL_H_

#include <deque>
#include "perfetto/base/logging.h"
#include "src/trace_processor/parser_types.h"

namespace perfetto {
namespace trace_processor {
namespace trace_sorter_internal {

// Moves object to the specified pointer and returns the pointer to the space
// behind it.
template <typename T>
char* AppendUnchecked(char* ptr, T value) {
  PERFETTO_DCHECK(reinterpret_cast<uintptr_t>(ptr) % alignof(T) == 0);
  new (ptr) T(std::move(value));
  return ptr + sizeof(T);
}

// Evicts object the the specified pointer, which now points to the space behind
// it.
template <typename T>
T EvictUnchecked(char** ptr) {
  PERFETTO_DCHECK(reinterpret_cast<uintptr_t>(*ptr) % alignof(T) == 0);
  T* type_ptr = reinterpret_cast<T*>(*ptr);
  T out(std::move(*type_ptr));
  type_ptr->~T();
  *ptr += sizeof(T);
  return out;
}

// Stores details of TrackEventData: presence of attributes and the
// lenght of the array.
struct TrackEventDataDescriptor {
 public:
  static constexpr uint64_t kBitsForCounterValues = 4;
  static constexpr uint64_t kThreadTimestampMask =
      1 << (kBitsForCounterValues + 1);
  static constexpr uint64_t kThreadInstructionCountMask =
      1 << kBitsForCounterValues;

  TrackEventDataDescriptor(bool has_thread_timestamp,
                           bool has_thread_instruction_count,
                           uint64_t number_of_counter_values)
      : packed_value_(GetPacketValue(has_thread_timestamp,
                                     has_thread_instruction_count,
                                     number_of_counter_values)) {
    PERFETTO_DCHECK(number_of_counter_values <=
                    TrackEventData::kMaxNumExtraCounters);
  }

  explicit TrackEventDataDescriptor(const TrackEventData& ted)
      : TrackEventDataDescriptor(ted.thread_timestamp.has_value(),
                                 ted.thread_instruction_count.has_value(),
                                 CountNumberOfCounterValues(ted)) {
    static_assert(
        TrackEventData::kMaxNumExtraCounters < (1 << kBitsForCounterValues),
        "kMaxNumExtraCounters can't be compressed properly");
  }

  static uint64_t CountNumberOfCounterValues(const TrackEventData& ted) {
    uint32_t num = 0;
    for (; num < TrackEventData::kMaxNumExtraCounters; ++num) {
      if (std::equal_to<double>()(ted.extra_counter_values[num], 0)) {
        break;
      }
    }
    return num;
  }

  static uint64_t GetPacketValue(bool has_thread_timestamp,
                                 bool has_thread_instruction_count,
                                 uint64_t number_of_counter_values) {
    return (static_cast<uint64_t>(has_thread_timestamp)
            << (kBitsForCounterValues + 1)) |
           (static_cast<uint64_t>(has_thread_instruction_count)
            << kBitsForCounterValues) |
           number_of_counter_values;
  }

  bool HasThreadTimestamp() const {
    return static_cast<bool>(packed_value_ & kThreadTimestampMask);
  }

  bool HasThreadInstructionCount() const {
    return static_cast<bool>(packed_value_ & kThreadInstructionCountMask);
  }

  uint64_t NumberOfCounterValues() const {
    return static_cast<uint64_t>(
        packed_value_ & static_cast<uint64_t>(~(3 << kBitsForCounterValues)));
  }

  uint64_t AppendedSize() const {
    return sizeof(TracePacketData) +
           8l * (/*counter_value*/ 1 + HasThreadTimestamp() +
                 HasThreadInstructionCount() + NumberOfCounterValues());
  }

 private:
  // uint8_t would be enough to hold all of the required data, but we need 8
  // bytes type for alignment.
  uint64_t packed_value_ = 0;
};

// Adds and removes object of the type from queue memory. Can be overriden
// for more specific functionality related to a type. All child classes
// should implement the same interface.
template <typename T>
class TypedMemoryAccessor {
 public:
  static char* Append(char* ptr, T value) {
    return AppendUnchecked(ptr, std::move(value));
  }
  static T Evict(char* ptr) { return EvictUnchecked<T>(&ptr); }
  static uint64_t AppendSize(const T&) {
    return static_cast<uint64_t>(sizeof(T));
  }
};

// Responsibe for accessing memory in the queue related to TrackEventData.
// Appends the struct more efficiently by compressing and decompressing some
// of TrackEventData attributes.
template <>
class TypedMemoryAccessor<TrackEventData> {
 public:
  static char* Append(char* ptr, TrackEventData ted) {
    auto ted_desc = TrackEventDataDescriptor(ted);
    ptr = AppendUnchecked(ptr, ted_desc);
    ptr = AppendUnchecked(ptr, TracePacketData{std::move(ted.packet),
                                               std::move(ted.sequence_state)});
    ptr = AppendUnchecked(ptr, ted.counter_value);
    if (ted_desc.HasThreadTimestamp()) {
      ptr = AppendUnchecked(ptr, ted.thread_timestamp.value());
    }
    if (ted_desc.HasThreadInstructionCount()) {
      ptr = AppendUnchecked(ptr, ted.thread_instruction_count.value());
    }
    for (uint32_t i = 0; i < ted_desc.NumberOfCounterValues(); i++) {
      ptr = AppendUnchecked(ptr, ted.extra_counter_values[i]);
    }
    return ptr;
  }

  static TrackEventData Evict(char* ptr) {
    auto ted_desc = EvictUnchecked<TrackEventDataDescriptor>(&ptr);
    TrackEventData ted(EvictUnchecked<TracePacketData>(&ptr));
    ted.counter_value = EvictUnchecked<double>(&ptr);
    if (ted_desc.HasThreadTimestamp()) {
      ted.thread_timestamp = EvictUnchecked<int64_t>(&ptr);
    }
    if (ted_desc.HasThreadInstructionCount()) {
      ted.thread_instruction_count = EvictUnchecked<int64_t>(&ptr);
    }
    for (uint32_t i = 0; i < ted_desc.NumberOfCounterValues(); i++) {
      ted.extra_counter_values[i] = EvictUnchecked<double>(&ptr);
    }
    return ted;
  }

  static uint64_t AppendSize(const TrackEventData& value) {
    return static_cast<uint64_t>(sizeof(TrackEventDataDescriptor)) +
           TrackEventDataDescriptor(value).AppendedSize();
  }
};

}  // namespace trace_sorter_internal
}  // namespace trace_processor
}  // namespace perfetto
#endif  // SRC_TRACE_PROCESSOR_TRACE_SORTER_INTERNAL_H_
