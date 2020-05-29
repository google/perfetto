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

RingBuffer::RingBuffer() {
  static_assert((kCapacity & (kCapacity - 1)) == 0,
                "Capacity should be a power of 2");
}

void RingBuffer::ReadAll(std::function<void(Record*)> fn) {
  // Mark as reading so we don't get reentrancy in obtaining new
  // trace events.
  is_reading_ = true;

  uint64_t start = (write_idx_ - start_idx_) < kCapacity
                       ? start_idx_
                       : write_idx_ - kCapacity;
  uint64_t end = write_idx_;

  // Increment the write index by kCapacity + 1. This ensures that if
  // ScopedEntry is destoryed in |fn| below, we won't get overwrites
  // while reading the buffer.
  // This works because of the logic in ~ScopedEntry and
  // RingBuffer::HasOverwritten which ensures that we don't overwrite entries
  // more than kCapcity elements in the past.
  write_idx_ += kCapacity + 1;

  for (uint64_t i = start; i < end; ++i) {
    Record* record = At(i);

    // If the slice was unfinished for some reason, don't emit it.
    if (record->duration_ns != 0) {
      fn(record);
    }
  }

  // Ensure that the start pointer is updated to the write pointer.
  start_idx_ = write_idx_;

  // Remove the reading marker.
  is_reading_ = false;
}

}  // namespace metatrace
}  // namespace trace_processor
}  // namespace perfetto
