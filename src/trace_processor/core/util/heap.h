/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_CORE_UTIL_HEAP_H_
#define SRC_TRACE_PROCESSOR_CORE_UTIL_HEAP_H_

#include <cstddef>

#include "perfetto/base/compiler.h"

// A binary heap in an array, with the element which is not `later` than any
// other on top: the same layout as std::make_heap() with `later` as its
// comparator. Unlike std::push_heap() and std::pop_heap(), these place a value
// in one pass which moves the elements it passes over, and do not swap it
// through a temporary at each level.

namespace perfetto::trace_processor::core {

// Places `value` in `heap[0, size)`, whose top is free: for replacing the top,
// or for removing it with the last element as `value`.
template <typename T, typename Later>
PERFETTO_ALWAYS_INLINE void HeapSiftDown(T* heap,
                                         size_t size,
                                         T value,
                                         const Later& later) {
  size_t at = 0;
  for (size_t child = 1; child < size; child = (2 * at) + 1) {
    if (child + 1 < size && later(heap[child], heap[child + 1])) {
      ++child;
    }
    if (!later(value, heap[child])) {
      break;
    }
    heap[at] = heap[child];
    at = child;
  }
  heap[at] = value;
}

// Places `value` in the heap `heap[0, at]`, whose last element `heap[at]` is
// free: for adding one.
template <typename T, typename Later>
PERFETTO_ALWAYS_INLINE void HeapSiftUp(T* heap,
                                       size_t at,
                                       T value,
                                       const Later& later) {
  while (at > 0) {
    size_t parent = (at - 1) / 2;
    if (!later(heap[parent], value)) {
      break;
    }
    heap[at] = heap[parent];
    at = parent;
  }
  heap[at] = value;
}

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_UTIL_HEAP_H_
