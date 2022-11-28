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

#ifndef SRC_TRACE_PROCESSOR_SORTER_TRACE_SORTER_INTERNAL_H_
#define SRC_TRACE_PROCESSOR_SORTER_TRACE_SORTER_INTERNAL_H_

#include <deque>

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/parser_types.h"

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
    for (uint32_t i = 0; i < TrackEventData::kMaxNumExtraCounters; ++i) {
      if (std::equal_to<double>()(ted.extra_counter_values[i], 0)) {
        return i;
      }
    }
    return TrackEventData::kMaxNumExtraCounters;
  }

  static constexpr uint64_t GetPacketValue(bool has_thread_timestamp,
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

struct AppendOptions {
  bool skip_trace_blob_view;
  bool skip_sequence_state;
};

struct EvictSkippedFields {
  base::Optional<TraceBlobView> skipped_trace_blob_view;
  base::Optional<RefPtr<PacketSequenceStateGeneration>> skipped_sequence_state;
};

// Adds and removes object of the type from queue memory. Can be overriden
// for more specific functionality related to a type. All child classes
// should implement the same interface.
template <typename T>
class TypedMemoryAccessor {
 public:
  static char* Append(char* ptr, T value, AppendOptions options) {
    PERFETTO_DCHECK(!options.skip_trace_blob_view);
    PERFETTO_DCHECK(!options.skip_sequence_state);
    return AppendUnchecked(ptr, std::move(value));
  }
  static T Evict(char* ptr, EvictSkippedFields options) {
    PERFETTO_DCHECK(!options.skipped_trace_blob_view);
    PERFETTO_DCHECK(!options.skipped_sequence_state);
    return EvictUnchecked<T>(&ptr);
  }
  static uint64_t AppendSize(const T&) {
    return static_cast<uint64_t>(sizeof(T));
  }

  static base::Optional<TraceBlobView> GetTraceBlobView(const T&) {
    return base::nullopt;
  }
  static base::Optional<RefPtr<PacketSequenceStateGeneration>> GetSequenceState(
      const T&) {
    return base::nullopt;
  }
};

// Responsibe for accessing memory in the queue related to TrackEventData.
// Appends the struct more efficiently by compressing and decompressing some
// of TrackEventData attributes.
template <>
class TypedMemoryAccessor<TrackEventData> {
 public:
  static char* Append(char* ptr, TrackEventData ted, AppendOptions options) {
    auto ted_desc = TrackEventDataDescriptor(ted);
    ptr = AppendUnchecked(ptr, ted_desc);

    TraceBlobView& packet = ted.trace_packet_data.packet;
    if (options.skip_trace_blob_view) {
      // Noop: keep this empty branch to keep consistency with Evict.
    } else {
      ptr = AppendUnchecked<TraceBlobView>(ptr, std::move(packet));
    }
    if (options.skip_sequence_state) {
      // Noop: keep this empty branch to keep consistency with Evict.
    } else {
      ptr = AppendUnchecked<RefPtr<PacketSequenceStateGeneration>>(
          ptr, ted.trace_packet_data.sequence_state);
    }
    ptr = AppendUnchecked<double>(ptr, ted.counter_value);
    if (ted_desc.HasThreadTimestamp()) {
      ptr = AppendUnchecked<int64_t>(ptr, ted.thread_timestamp.value());
    }
    if (ted_desc.HasThreadInstructionCount()) {
      ptr = AppendUnchecked<int64_t>(ptr, ted.thread_instruction_count.value());
    }
    for (uint32_t i = 0; i < ted_desc.NumberOfCounterValues(); i++) {
      ptr = AppendUnchecked<double>(ptr, ted.extra_counter_values[i]);
    }
    return ptr;
  }

  static TrackEventData Evict(char* ptr, EvictSkippedFields fields) {
    auto ted_desc = EvictUnchecked<TrackEventDataDescriptor>(&ptr);

    TrackEventData ted({}, {});
    if (fields.skipped_trace_blob_view) {
      ted.trace_packet_data.packet = std::move(*fields.skipped_trace_blob_view);
    } else {
      ted.trace_packet_data.packet = EvictUnchecked<TraceBlobView>(&ptr);
    }
    if (fields.skipped_sequence_state) {
      ted.trace_packet_data.sequence_state =
          std::move(*fields.skipped_sequence_state);
    } else {
      ted.trace_packet_data.sequence_state =
          EvictUnchecked<RefPtr<PacketSequenceStateGeneration>>(&ptr);
    }

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

  static TraceBlobView GetTraceBlobView(const TrackEventData& ted) {
    return ted.trace_packet_data.packet.copy();
  }

  static RefPtr<PacketSequenceStateGeneration> GetSequenceState(
      const TrackEventData& ted) {
    return ted.trace_packet_data.sequence_state;
  }

  static uint64_t AppendSize(const TrackEventData& value) {
    return static_cast<uint64_t>(sizeof(TrackEventDataDescriptor)) +
           TrackEventDataDescriptor(value).AppendedSize();
  }
};

}  // namespace trace_sorter_internal
}  // namespace trace_processor
}  // namespace perfetto
#endif  // SRC_TRACE_PROCESSOR_SORTER_TRACE_SORTER_INTERNAL_H_
