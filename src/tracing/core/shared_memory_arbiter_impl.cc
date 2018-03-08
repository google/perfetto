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

#include "src/tracing/core/shared_memory_arbiter_impl.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/tracing/core/commit_data_request.h"
#include "perfetto/tracing/core/shared_memory.h"
#include "src/tracing/core/trace_writer_impl.h"

#include <limits>
#include <utility>

namespace perfetto {

using Chunk = SharedMemoryABI::Chunk;

// static
SharedMemoryABI::PageLayout SharedMemoryArbiterImpl::default_page_layout =
    SharedMemoryABI::PageLayout::kPageDiv1;

// static
std::unique_ptr<SharedMemoryArbiter> SharedMemoryArbiter::CreateInstance(
    SharedMemory* shared_memory,
    size_t page_size,
    Service::ProducerEndpoint* producer_endpoint,
    base::TaskRunner* task_runner) {
  return std::unique_ptr<SharedMemoryArbiterImpl>(
      new SharedMemoryArbiterImpl(shared_memory->start(), shared_memory->size(),
                                  page_size, producer_endpoint, task_runner));
}

SharedMemoryArbiterImpl::SharedMemoryArbiterImpl(
    void* start,
    size_t size,
    size_t page_size,
    Service::ProducerEndpoint* producer_endpoint,
    base::TaskRunner* task_runner)
    : task_runner_(task_runner),
      producer_endpoint_(producer_endpoint),
      shmem_abi_(reinterpret_cast<uint8_t*>(start), size, page_size),
      active_writer_ids_(kMaxWriterID),
      weak_ptr_factory_(this) {}

Chunk SharedMemoryArbiterImpl::GetNewChunk(
    const SharedMemoryABI::ChunkHeader& header,
    size_t size_hint) {
  PERFETTO_DCHECK(size_hint == 0);  // Not implemented yet.
  int stall_count = 0;
  const useconds_t kStallIntervalUs = 100000;

  for (;;) {
    // TODO(primiano): Probably this lock is not really required and this code
    // could be rewritten leveraging only the Try* atomic operations in
    // SharedMemoryABI. But let's not be too adventurous for the moment.
    {
      std::lock_guard<std::mutex> scoped_lock(lock_);
      const size_t initial_page_idx = page_idx_;
      for (size_t i = 0; i < shmem_abi_.num_pages(); i++) {
        page_idx_ = (initial_page_idx + i) % shmem_abi_.num_pages();
        bool is_new_page = false;

        // TODO(primiano): make the page layout dynamic.
        auto layout = SharedMemoryArbiterImpl::default_page_layout;

        if (shmem_abi_.is_page_free(page_idx_)) {
          // TODO(primiano): Use the |size_hint| here to decide the layout.
          is_new_page = shmem_abi_.TryPartitionPage(page_idx_, layout);
        }
        uint32_t free_chunks;
        if (is_new_page) {
          free_chunks = (1 << SharedMemoryABI::kNumChunksForLayout[layout]) - 1;
        } else {
          free_chunks = shmem_abi_.GetFreeChunks(page_idx_);
        }

        for (uint32_t chunk_idx = 0; free_chunks;
             chunk_idx++, free_chunks >>= 1) {
          if (!(free_chunks & 1))
            continue;
          // We found a free chunk.
          Chunk chunk = shmem_abi_.TryAcquireChunkForWriting(
              page_idx_, chunk_idx, &header);
          if (!chunk.is_valid())
            continue;
          PERFETTO_DLOG("Acquired chunk %zu:%u", page_idx_, chunk_idx);
          if (stall_count) {
            PERFETTO_LOG(
                "Recovered from stall after %" PRIu64 " ms",
                static_cast<uint64_t>(kStallIntervalUs * stall_count / 1000));
          }
          return chunk;
        }
      }
    }  // std::lock_guard<std::mutex>

    // All chunks are taken (either kBeingWritten by us or kBeingRead by the
    // Service). TODO: at this point we should return a bankrupcy chunk, not
    // crash the process.
    if (stall_count++ == 0) {
      PERFETTO_ELOG("Shared memory buffer overrun! Stalling");

      // TODO(primiano): sending the IPC synchronously is a temporary workaround
      // until the backpressure logic in probes_producer is sorted out. Until
      // then the risk is that we stall the message loop waiting for the
      // tracing service to  consume the shared memory buffer (SMB) and, for
      // this reason, never run the task that tells the service to purge the
      // SMB.
      // TODO(primiano): We cannot call this if we aren't on the |task_runner_|
      // thread. Works for now because all traced_probes writes happen on the
      // main thread.
      SendPendingCommitDataRequest();
    }
    usleep(kStallIntervalUs);
  }
}

void SharedMemoryArbiterImpl::ReturnCompletedChunk(Chunk chunk,
                                                   BufferID target_buffer) {
  bool should_post_callback = false;
  base::WeakPtr<SharedMemoryArbiterImpl> weak_this;
  {
    std::lock_guard<std::mutex> scoped_lock(lock_);
    uint8_t chunk_idx = chunk.chunk_idx();
    size_t page_idx = shmem_abi_.ReleaseChunkAsComplete(std::move(chunk));
    if (page_idx != SharedMemoryABI::kInvalidPageIdx) {
      if (!commit_data_req_) {
        commit_data_req_.reset(new CommitDataRequest());
        weak_this = weak_ptr_factory_.GetWeakPtr();
        should_post_callback = true;
      }
      CommitDataRequest::ChunksToMove* ctm =
          commit_data_req_->add_chunks_to_move();
      ctm->set_page(static_cast<uint32_t>(page_idx));
      ctm->set_chunk(chunk_idx);
      ctm->set_target_buffer(target_buffer);
    }
  }

  if (should_post_callback) {
    PERFETTO_DCHECK(weak_this);
    task_runner_->PostTask([weak_this] {
      if (weak_this)
        weak_this->SendPendingCommitDataRequest();
    });
  }
}

// This is always invoked on the |task_runner_| thread.
void SharedMemoryArbiterImpl::SendPendingCommitDataRequest() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  std::unique_ptr<CommitDataRequest> req;
  std::vector<uint32_t> pages_to_notify;
  {
    std::lock_guard<std::mutex> scoped_lock(lock_);
    req = std::move(commit_data_req_);
  }
  // |commit_data_req_| could become nullptr if the forced sync flush happens
  // in GetNewChunk().
  if (req)
    producer_endpoint_->CommitData(*req);
}

std::unique_ptr<TraceWriter> SharedMemoryArbiterImpl::CreateTraceWriter(
    BufferID target_buffer) {
  WriterID id;
  {
    std::lock_guard<std::mutex> scoped_lock(lock_);
    id = active_writer_ids_.Allocate();
  }
  return std::unique_ptr<TraceWriter>(
      id ? new TraceWriterImpl(this, id, target_buffer) : nullptr);
}

void SharedMemoryArbiterImpl::ReleaseWriterID(WriterID id) {
  std::lock_guard<std::mutex> scoped_lock(lock_);
  active_writer_ids_.Free(id);
}

}  // namespace perfetto
