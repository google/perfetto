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
#include "perfetto/tracing/core/shared_memory.h"
#include "src/tracing/core/trace_writer_impl.h"

#include <limits>

namespace perfetto {

using Chunk = SharedMemoryABI::Chunk;

// static
SharedMemoryABI::PageLayout SharedMemoryArbiterImpl::default_page_layout =
    SharedMemoryABI::PageLayout::kPageDiv1;

// static
std::unique_ptr<SharedMemoryArbiter> SharedMemoryArbiter::CreateInstance(
    SharedMemory* shared_memory,
    size_t page_size,
    OnPagesCompleteCallback callback,
    base::TaskRunner* task_runner) {
  return std::unique_ptr<SharedMemoryArbiterImpl>(
      new SharedMemoryArbiterImpl(shared_memory->start(), shared_memory->size(),
                                  page_size, callback, task_runner));
}
SharedMemoryArbiterImpl::SharedMemoryArbiterImpl(
    void* start,
    size_t size,
    size_t page_size,
    OnPagesCompleteCallback callback,
    base::TaskRunner* task_runner)
    : task_runner_(task_runner),
      on_pages_complete_callback_(std::move(callback)),
      shmem_abi_(reinterpret_cast<uint8_t*>(start), size, page_size),
      active_writer_ids_(SharedMemoryABI::kMaxWriterID) {}

Chunk SharedMemoryArbiterImpl::GetNewChunk(
    const SharedMemoryABI::ChunkHeader& header,
    BufferID target_buffer,
    size_t size_hint) {
  PERFETTO_DCHECK(size_hint == 0);  // Not implemented yet.

  for (;;) {
    // TODO(primiano): Probably this lock is not really required and this code
    // could be rewritten leveraging only the Try* atomic operations in
    // SharedMemoryABI. But let's not be too adventurous for the moment.
    {
      std::lock_guard<std::mutex> scoped_lock(lock_);
      const size_t initial_page_idx = page_idx_;
      // TODO(primiano): instead of scanning, we could maintain a bitmap of
      // free chunks for each |target_buffer| and one for fully free pages.
      for (size_t i = 0; i < shmem_abi_.num_pages(); i++) {
        page_idx_ = (initial_page_idx + i) % shmem_abi_.num_pages();
        bool is_new_page = false;

        // TODO(primiano): make the page layout dynamic.
        auto layout = SharedMemoryArbiterImpl::default_page_layout;

        if (shmem_abi_.is_page_free(page_idx_)) {
          // TODO(primiano): Use the |size_hint| here to decide the layout.
          is_new_page =
              shmem_abi_.TryPartitionPage(page_idx_, layout, target_buffer);
        }
        uint32_t free_chunks;
        size_t tbuf;
        if (is_new_page) {
          free_chunks = (1 << SharedMemoryABI::kNumChunksForLayout[layout]) - 1;
          tbuf = target_buffer;
        } else {
          free_chunks = shmem_abi_.GetFreeChunks(page_idx_);

          // |tbuf| here is advisory only and could change at any point, before
          // or after this read. The only use of |tbuf| here is to to skip pages
          // that are more likely to belong to other target_buffers, avoiding
          // the more epxensive atomic operations in those cases. The
          // authoritative check on |tbuf| happens atomically in the
          // TryAcquireChunkForWriting() call below.
          tbuf = shmem_abi_.page_header(page_idx_)->target_buffer.load(
              std::memory_order_relaxed);
        }
        PERFETTO_DLOG("Free chunks for page %zu: %x. Target buffer: %zu",
                      page_idx_, free_chunks, tbuf);

        if (tbuf != target_buffer)
          continue;

        for (uint32_t chunk_idx = 0; free_chunks;
             chunk_idx++, free_chunks >>= 1) {
          if (!(free_chunks & 1))
            continue;
          // We found a free chunk.
          Chunk chunk = shmem_abi_.TryAcquireChunkForWriting(
              page_idx_, chunk_idx, tbuf, &header);
          if (!chunk.is_valid())
            continue;
          PERFETTO_DLOG("Acquired chunk %zu:%u", page_idx_, chunk_idx);
          return chunk;
        }
        // TODO: we should have some policy to guarantee fairness of the SMB
        // page allocator w.r.t |target_buffer|? Or is the SMB best-effort. All
        // chunks in the page are busy (either kBeingRead or kBeingWritten), or
        // all the pages are assigned to a different target buffer. Try with the
        // next page.
      }
    }  // std::lock_guard<std::mutex>
    // All chunks are taken (either kBeingWritten by us or kBeingRead by the
    // Service). TODO: at this point we should return a bankrupcy chunk, not
    // crash the process.
    PERFETTO_ELOG("Shared memory buffer overrun! Stalling");
    usleep(250000);
  }
}

void SharedMemoryArbiterImpl::ReturnCompletedChunk(Chunk chunk) {
  bool should_post_callback = false;
  {
    std::lock_guard<std::mutex> scoped_lock(lock_);
    size_t page_index = shmem_abi_.ReleaseChunkAsComplete(std::move(chunk));
    if (page_index != SharedMemoryABI::kInvalidPageIdx) {
      should_post_callback = pages_to_notify_.empty();
      pages_to_notify_.push_back(static_cast<uint32_t>(page_index));
    }
  }
  if (should_post_callback) {
    // TODO what happens if the arbiter gets destroyed?
    task_runner_->PostTask(std::bind(
        &SharedMemoryArbiterImpl::InvokeOnPagesCompleteCallback, this));
  }
}

// This is always invoked on the |task_runner_| thread.
void SharedMemoryArbiterImpl::InvokeOnPagesCompleteCallback() {
  std::vector<uint32_t> pages_to_notify;
  {
    std::lock_guard<std::mutex> scoped_lock(lock_);
    pages_to_notify = std::move(pages_to_notify_);
    pages_to_notify_.clear();
  }
  on_pages_complete_callback_(pages_to_notify);
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
