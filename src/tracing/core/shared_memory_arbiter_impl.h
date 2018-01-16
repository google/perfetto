/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_TRACING_CORE_SHARED_MEMORY_ARBITER_IMPL_H_
#define SRC_TRACING_CORE_SHARED_MEMORY_ARBITER_IMPL_H_

#include <stdint.h>

#include <functional>
#include <mutex>
#include <vector>

#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/shared_memory_abi.h"
#include "perfetto/tracing/core/shared_memory_arbiter.h"
#include "src/tracing/core/id_allocator.h"

namespace perfetto {

class TraceWriter;

namespace base {
class TaskRunner;
}  // namespace base

// This class handles the shared memory buffer on the producer side. It is used
// to obtain thread-local chunks and to partition pages from several threads.
// There is one arbiter instance per Producer.
// This class is thread-safe and uses locks to do so. Data sources are supposed
// to interact with this sporadically, only when they run out of space on their
// current thread-local chunk.
class SharedMemoryArbiterImpl : public SharedMemoryArbiter {
 public:
  // Args:
  // |start|,|size|: boundaries of the shared memory buffer.
  // |page_size|: a multiple of 4KB that defines the granularity of tracing
  // pages. See tradeoff considerations in shared_memory_abi.h.
  // |OnPagesCompleteCallback|: a callback that will be posted on the passed
  // |TaskRunner| when one or more pages are complete (and hence the Producer
  // should send a NotifySharedMemoryUpdate() to the Service).
  SharedMemoryArbiterImpl(void* start,
                          size_t size,
                          size_t page_size,
                          OnPagesCompleteCallback,
                          base::TaskRunner*);

  // Returns a new Chunk to write tracing data. The call always returns a valid
  // Chunk. TODO(primiano): right now this blocks if there are no free chunks
  // in the SMB. In the long term the caller should be allowed to pick a policy
  // and handle the retry itself asynchronously.
  SharedMemoryABI::Chunk GetNewChunk(const SharedMemoryABI::ChunkHeader&,
                                     BufferID target_buffer,
                                     size_t size_hint = 0);

  void ReturnCompletedChunk(SharedMemoryABI::Chunk chunk);

  SharedMemoryABI* shmem_abi_for_testing() { return &shmem_abi_; }

  static void set_default_layout_for_testing(SharedMemoryABI::PageLayout l) {
    default_page_layout = l;
  }

  // SharedMemoryArbiter implementation.
  // See include/perfetto/tracing/core/shared_memory_arbiter.h for comments.
  std::unique_ptr<TraceWriter> CreateTraceWriter(
      BufferID target_buffer = 0) override;

 private:
  friend class TraceWriterImpl;

  static SharedMemoryABI::PageLayout default_page_layout;

  SharedMemoryArbiterImpl(const SharedMemoryArbiterImpl&) = delete;
  SharedMemoryArbiterImpl& operator=(const SharedMemoryArbiterImpl&) = delete;

  // Called by the TraceWriter destructor.
  void ReleaseWriterID(WriterID);

  void InvokeOnPagesCompleteCallback();

  base::TaskRunner* const task_runner_;
  OnPagesCompleteCallback on_pages_complete_callback_;

  // --- Begin lock-protected members ---
  std::mutex lock_;
  SharedMemoryABI shmem_abi_;
  size_t page_idx_ = 0;
  IdAllocator<WriterID> active_writer_ids_;
  std::vector<uint32_t> pages_to_notify_;
  // --- End lock-protected members ---
};

}  // namespace perfetto

#endif  // SRC_TRACING_CORE_SHARED_MEMORY_ARBITER_IMPL_H_
