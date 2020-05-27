/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/tp_metatrace.h"

namespace perfetto {
namespace trace_processor {
namespace metatrace {

bool g_enabled = false;

void Enable() {
  g_enabled = true;
}

void DisableAndReadBuffer(std::function<void(Record*)> fn) {
  g_enabled = false;
  if (!fn)
    return;
  RingBuffer::GetInstance()->ReadAll(fn);
}

RingBuffer::RingBuffer() : data_{} {
  static_assert((kCapacity & (kCapacity - 1)) == 0,
                "Capacity should be a power of 2");
}

void RingBuffer::ReadAll(std::function<void(Record*)> fn) {
  // Mark as reading so we don't get reentrancy in obtaining new
  // trace events.
  is_reading_ = true;

  uint64_t start = write_idx_ < kCapacity ? 0 : write_idx_ - kCapacity;
  uint64_t end = write_idx_;
  for (uint64_t i = start; i < end; ++i) {
    Record* record = At(i);

    // Increment the generation number so that we won't overwrite this entry
    // if there's accidental reentrancy which destroys the ScopedEntry.
    record->generation++;

    // If the slice was unfinished for some reason, don't emit it.
    if (record->duration_ns != 0) {
      fn(record);
    }
  }

  // Reset the write pointer so we start at the beginning of the ring buffer
  // if we reenable metatracing.
  write_idx_ = 0;

  // Remove the reading marker.
  is_reading_ = false;
}

}  // namespace metatrace
}  // namespace trace_processor
}  // namespace perfetto
