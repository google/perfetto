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
#include <memory>
#include <mutex>
#include <vector>

#include "perfetto/base/thread_checker.h"
#include "perfetto/base/weak_ptr.h"
#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/shared_memory_abi.h"
#include "perfetto/tracing/core/shared_memory_arbiter.h"
#include "src/tracing/core/id_allocator.h"

namespace perfetto {

class CommitDataRequest;
class PatchList;
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
  // should send a CommitData request to the Service).
  SharedMemoryArbiterImpl(void* start,
                          size_t size,
                          size_t page_size,
                          TracingService::ProducerEndpoint*,
                          base::TaskRunner*);

  // Returns a new Chunk to write tracing data. The call always returns a valid
  // Chunk. TODO(primiano): right now this blocks if there are no free chunks
  // in the SMB. In the long term the caller should be allowed to pick a policy
  // and handle the retry itself asynchronously.
  SharedMemoryABI::Chunk GetNewChunk(const SharedMemoryABI::ChunkHeader&,
                                     size_t size_hint = 0);

  // Puts back a Chunk that has been completed and sends a request to the
  // service to move it to the central tracing buffer. |target_buffer| is the
  // absolute trace buffer ID where the service should move the chunk onto (the
  // producer is just to copy back the same number received in the
  // DataSourceConfig upon the CreateDataSourceInstance() reques).
  // PatchList is a pointer to the list of patches for previous chunks. The
  // first patched entries will be removed from the patched list and sent over
  // to the service in the same CommitData() IPC request.
  void ReturnCompletedChunk(SharedMemoryABI::Chunk,
                            BufferID target_buffer,
                            PatchList*);

  // Forces a synchronous commit of the completed packets without waiting for
  // the next task.
  void FlushPendingCommitDataRequests(std::function<void()> callback = {});

  SharedMemoryABI* shmem_abi_for_testing() { return &shmem_abi_; }

  static void set_default_layout_for_testing(SharedMemoryABI::PageLayout l) {
    default_page_layout = l;
  }

  // SharedMemoryArbiter implementation.
  // See include/perfetto/tracing/core/shared_memory_arbiter.h for comments.
  std::unique_ptr<TraceWriter> CreateTraceWriter(
      BufferID target_buffer = 0) override;

  void NotifyFlushComplete(FlushRequestID) override;

 private:
  friend class TraceWriterImpl;

  static SharedMemoryABI::PageLayout default_page_layout;

  SharedMemoryArbiterImpl(const SharedMemoryArbiterImpl&) = delete;
  SharedMemoryArbiterImpl& operator=(const SharedMemoryArbiterImpl&) = delete;

  // Called by the TraceWriter destructor.
  void ReleaseWriterID(WriterID);

  base::TaskRunner* const task_runner_;
  TracingService::ProducerEndpoint* const producer_endpoint_;
  PERFETTO_THREAD_CHECKER(thread_checker_)

  // --- Begin lock-protected members ---
  std::mutex lock_;
  SharedMemoryABI shmem_abi_;
  size_t page_idx_ = 0;
  std::unique_ptr<CommitDataRequest> commit_data_req_;
  size_t bytes_pending_commit_ = 0;  // SUM(chunk.size() : commit_data_req_).
  IdAllocator<WriterID> active_writer_ids_;
  // --- End lock-protected members ---

  // Keep at the end.
  base::WeakPtrFactory<SharedMemoryArbiterImpl> weak_ptr_factory_;
};

}  // namespace perfetto

#endif  // SRC_TRACING_CORE_SHARED_MEMORY_ARBITER_IMPL_H_
