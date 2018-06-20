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
#include "perfetto/base/time.h"
#include "perfetto/tracing/core/commit_data_request.h"
#include "perfetto/tracing/core/shared_memory.h"
#include "src/tracing/core/null_trace_writer.h"
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
    TracingService::ProducerEndpoint* producer_endpoint,
    base::TaskRunner* task_runner) {
  return std::unique_ptr<SharedMemoryArbiterImpl>(
      new SharedMemoryArbiterImpl(shared_memory->start(), shared_memory->size(),
                                  page_size, producer_endpoint, task_runner));
}

SharedMemoryArbiterImpl::SharedMemoryArbiterImpl(
    void* start,
    size_t size,
    size_t page_size,
    TracingService::ProducerEndpoint* producer_endpoint,
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
  unsigned stall_interval_us = 0;
  static const unsigned kMaxStallIntervalUs = 100000;
  static const int kLogAfterNStalls = 3;

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
          if (stall_count > kLogAfterNStalls) {
            PERFETTO_LOG("Recovered from stall after %d iterations",
                         stall_count);
          }
          return chunk;
        }
      }
    }  // std::lock_guard<std::mutex>

    // All chunks are taken (either kBeingWritten by us or kBeingRead by the
    // Service). TODO: at this point we should return a bankrupcy chunk, not
    // crash the process.
    if (stall_count++ == kLogAfterNStalls) {
      PERFETTO_ELOG("Shared memory buffer overrun! Stalling");

      // TODO(primiano): sending the IPC synchronously is a temporary workaround
      // until the backpressure logic in probes_producer is sorted out. Until
      // then the risk is that we stall the message loop waiting for the
      // tracing service to  consume the shared memory buffer (SMB) and, for
      // this reason, never run the task that tells the service to purge the
      // SMB.
      FlushPendingCommitDataRequests();
    }
    base::SleepMicroseconds(stall_interval_us);
    stall_interval_us =
        std::min(kMaxStallIntervalUs, (stall_interval_us + 1) * 8);
  }
}

void SharedMemoryArbiterImpl::ReturnCompletedChunk(Chunk chunk,
                                                   BufferID target_buffer,
                                                   PatchList* patch_list) {
  bool should_post_callback = false;
  bool should_commit_synchronously = false;
  base::WeakPtr<SharedMemoryArbiterImpl> weak_this;
  {
    std::lock_guard<std::mutex> scoped_lock(lock_);
    uint8_t chunk_idx = chunk.chunk_idx();
    const WriterID writer_id = chunk.writer_id();
    bytes_pending_commit_ += chunk.size();
    size_t page_idx = shmem_abi_.ReleaseChunkAsComplete(std::move(chunk));

    // DO NOT access |chunk| after this point, has been std::move()-d above.

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

    // If more than half of the SMB.size() is filled with completed chunks for
    // which we haven't notified the service yet (i.e. they are still enqueued
    // in |commit_data_req_|), force a synchronous CommitDataRequest(), to
    // reduce the likeliness of stalling the writer.
    if (bytes_pending_commit_ >= shmem_abi_.size() / 2) {
      should_commit_synchronously = true;
      should_post_callback = false;
    }

    // Get the patches completed for the previous chunk from the |patch_list|
    // and update it.
    ChunkID last_chunk_id = 0;  // 0 is irrelevant but keeps the compiler happy.
    CommitDataRequest::ChunkToPatch* last_chunk_req = nullptr;
    while (!patch_list->empty() && patch_list->front().is_patched()) {
      if (!last_chunk_req || last_chunk_id != patch_list->front().chunk_id) {
        last_chunk_req = commit_data_req_->add_chunks_to_patch();
        last_chunk_req->set_writer_id(writer_id);
        last_chunk_id = patch_list->front().chunk_id;
        last_chunk_req->set_chunk_id(last_chunk_id);
        last_chunk_req->set_target_buffer(target_buffer);
      }
      auto* patch_req = last_chunk_req->add_patches();
      patch_req->set_offset(patch_list->front().offset);
      patch_req->set_data(&patch_list->front().size_field[0],
                          patch_list->front().size_field.size());
      patch_list->pop_front();
    }
    // Patches are enqueued in the |patch_list| in order and are notified to
    // the service when the chunk is returned. The only case when the current
    // patch list is incomplete is if there is an unpatched entry at the head of
    // the |patch_list| that belongs to the same ChunkID as the last one we are
    // about to send to the service.
    if (last_chunk_req && !patch_list->empty() &&
        patch_list->front().chunk_id == last_chunk_id) {
      last_chunk_req->set_has_more_patches(true);
    }
  }  // scoped_lock(lock_)

  if (should_post_callback) {
    PERFETTO_DCHECK(weak_this);
    task_runner_->PostTask([weak_this] {
      if (weak_this)
        weak_this->FlushPendingCommitDataRequests();
    });
  }

  if (should_commit_synchronously)
    FlushPendingCommitDataRequests();
}

// TODO(primiano): this is wrong w.r.t. threading because it will try to send
// an IPC from a different thread than the IPC thread. Right now this works
// because everything is single threaded. It will hit the thread checker
// otherwise. What we really want to do here is doing this sync IPC only if
// task_runner_.RunsTaskOnCurrentThread(), otherwise PostTask().
void SharedMemoryArbiterImpl::FlushPendingCommitDataRequests(
    std::function<void()> callback) {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  std::unique_ptr<CommitDataRequest> req;
  {
    std::lock_guard<std::mutex> scoped_lock(lock_);
    req = std::move(commit_data_req_);
    bytes_pending_commit_ = 0;
  }
  // |commit_data_req_| could become nullptr if the forced sync flush happens
  // in GetNewChunk().
  if (req) {
    producer_endpoint_->CommitData(*req, callback);
  } else if (callback) {
    // If |commit_data_req_| was nullptr, it means that an enqueued deferred
    // commit was executed just before this. At this point send an empty commit
    // request to the service, just to linearize with it and give the guarantee
    // to the caller that the data has been flushed into the service.
    producer_endpoint_->CommitData(CommitDataRequest(), callback);
  }
}

std::unique_ptr<TraceWriter> SharedMemoryArbiterImpl::CreateTraceWriter(
    BufferID target_buffer) {
  WriterID id;
  {
    std::lock_guard<std::mutex> scoped_lock(lock_);
    id = active_writer_ids_.Allocate();
  }
  if (!id)
    return std::unique_ptr<TraceWriter>(new NullTraceWriter());
  return std::unique_ptr<TraceWriter>(
      new TraceWriterImpl(this, id, target_buffer));
}

void SharedMemoryArbiterImpl::NotifyFlushComplete(FlushRequestID req_id) {
  bool should_post_commit_task = false;
  {
    std::lock_guard<std::mutex> scoped_lock(lock_);
    // If a commit_data_req_ exists it means that somebody else already posted a
    // FlushPendingCommitDataRequests() task.
    if (!commit_data_req_) {
      commit_data_req_.reset(new CommitDataRequest());
      should_post_commit_task = true;
    } else {
      // If there is another request queued and that also contains is a reply
      // to a flush request, reply with the highest id.
      req_id = std::max(req_id, commit_data_req_->flush_request_id());
    }
    commit_data_req_->set_flush_request_id(req_id);
  }
  if (should_post_commit_task) {
    auto weak_this = weak_ptr_factory_.GetWeakPtr();
    task_runner_->PostTask([weak_this] {
      if (weak_this)
        weak_this->FlushPendingCommitDataRequests();
    });
  }
}

void SharedMemoryArbiterImpl::ReleaseWriterID(WriterID id) {
  std::lock_guard<std::mutex> scoped_lock(lock_);
  active_writer_ids_.Free(id);
}

}  // namespace perfetto
