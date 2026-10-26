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

#ifndef SRC_TRACE_PROCESSOR_UTIL_CHUNK_MEMORY_H_
#define SRC_TRACE_PROCESSOR_UTIL_CHUNK_MEMORY_H_

#include <cstddef>
#include <cstdint>

#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "src/trace_processor/util/io_mode.h"

namespace perfetto::trace_processor {

// Where trace processor's bulk working memory (the sorter's token buffer)
// comes from. Chunks are heap blobs in every mode, so views of a chunk keep its
// memory alive after its owner drops it.
//
// On Linux, Android and Apple platforms, chunk contents can also be paged out
// to an unlinked temporary file and read back, so the owner can keep only a
// budget of chunks resident. Only whether there is somewhere to page to
// differs between modes.
class ChunkMemory : public RefCounted {
 public:
  static constexpr size_t kNoBudget = SIZE_MAX;

  virtual ~ChunkMemory();

  // The size of every chunk.
  size_t chunk_size() const { return chunk_size_; }

  // How many bytes of chunks the owner should keep resident; kNoBudget when
  // chunks cannot be paged out.
  size_t resident_budget() const { return resident_budget_; }

  // Returns a blob of chunk_size() writable bytes, 16-byte aligned.
  TraceBlob AllocateChunk() const { return TraceBlob::Allocate(chunk_size_); }

  // Writes |size| bytes at |data| out and returns the slot holding them. Only
  // called when there is a budget.
  virtual uint32_t PageOut(const uint8_t* data, size_t size);
  // Reads the contents of |slot| back into |data| and frees the slot.
  virtual void PageIn(uint32_t slot, uint8_t* data, size_t size);
  // Frees |slot| without reading it.
  virtual void FreePage(uint32_t slot);

  // The memory for |mode|: heap chunks, which IoMode::kNative can also page
  // out to a temporary file where the platform supports it.
  static RefPtr<ChunkMemory> Create(IoMode mode, size_t chunk_size);

  // Heap chunks which are never paged out.
  static RefPtr<ChunkMemory> Heap(size_t chunk_size);

  // Heap chunks paged out to a temporary file past |resident_budget| bytes;
  // null if this platform or process cannot make the file.
  static RefPtr<ChunkMemory> Paged(size_t chunk_size, size_t resident_budget);

 protected:
  ChunkMemory(size_t chunk_size, size_t resident_budget);

 private:
  const size_t chunk_size_;
  const size_t resident_budget_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_UTIL_CHUNK_MEMORY_H_
