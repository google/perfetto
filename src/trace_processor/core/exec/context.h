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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_CONTEXT_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_CONTEXT_H_

#include <cstdint>

#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

struct FilterValue;

// What one execution of a plan shares as it runs: the storage the batches'
// own columns are in. A step takes a buffer for each column it fills, sized
// for the column's values, and hands it to the batch; once no batch holds it,
// the buffer comes back to be taken again (see BufferPool).
//
// Must outlive every batch holding one of its buffers.
class Context {
 public:
  Context();
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  // A buffer no batch holds, for values `width` bytes wide (see ValueWidth),
  // holding whatever it last held.
  ColumnBuffer TakeBuffer(uint32_t width) { return buffers_.Take(width); }

  // The bytes of value storage taken for buffers so far, held or free.
  uint64_t allocated_buffer_bytes() const { return buffers_.allocated_bytes(); }

  // A shared buffer for columns with no values: every row is null and every
  // value is zero.
  const ColumnBuffer& NullBuffer();

  // The values the next run's filters compare with, by parameter, already
  // cast (see FilterValue). Nothing is copied: the caller keeps them alive and
  // unchanged until the run ends.
  void SetFilterValues(Span<const FilterValue> values) {
    filter_values_ = values;
  }
  Span<const FilterValue> filter_values() const { return filter_values_; }

 private:
  // Declared before `nulls_`, so it outlives it.
  BufferPool buffers_;
  ColumnBuffer nulls_;
  Span<const FilterValue> filter_values_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_CONTEXT_H_
