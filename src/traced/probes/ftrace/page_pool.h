/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License At
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACED_PROBES_FTRACE_PAGE_POOL_H_
#define SRC_TRACED_PROBES_FTRACE_PAGE_POOL_H_

#include <stdint.h>

#include <mutex>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/optional.h"
#include "perfetto/base/paged_memory.h"
#include "perfetto/base/thread_checker.h"
#include "perfetto/base/utils.h"

namespace perfetto {

// This class is a page pool tailored around the needs of the ftrace CpuReader.
// It has two responsibilities:
// 1) A cheap bump-pointer page allocator for the writing side of CpuReader.
// 2) A thread-safe producer/consumer queue to synchronize the read/write
//    threads of CpuReader.
// For context, CpuReader (and hence this class) is used on two threads:
// (1) A worker thread that writes into the buffer and (2) the main thread which
// reads all the content in big batches and turn them into protos.
// There is at most one thread writing and at most one thread reading. In rare
// circumstances they can be active At the same time.
// This class is optimized for the following use case:
// - Most of the times CpuReader wants to write 4096 bytes. In some rare cases
//   (read() during flush) it wants to write < 4096 bytes.
// - Even when it writes < 4096 bytes, CpuReader can figure out the size of the
//   payload from the ftrace header. We don't need extra tracking to tell how
//   much of each page is used.
// - Doing a syscall for each page write is overkill. In most occasions
//   CpuReader writes bursts of several pages in one go.
// - We can't really predict upfront how big the write bursts will be, hence we
//   cannot predict the size of the pool, unless we accept a very high bound.
//   In extreme, yet rare, conditions, CpuReader will read the whole per-cpu
//   ftrace buffer, while the reader is still reading the previous batch.
// - Write burst should not be too frequent, so once they are over it's worth
//   spending some extra cycles to release the memory.
// - The reader side always wants to read *all* the written pages in one batch.
//   While this happens though, the write might want to write more.
//
// The architecture of this class is as follows. Pages are organized in
// PageBlock(s). A PageBlock is simply an array of pages and is the elementary
// unit of memory allocation and frees. Pages within one block are cheaply
// allocated with a simple bump-pointer allocator.
//
//      [      Writer (thread worker)    ] | [    Reader (main thread)   ]
//                                  ~~~~~~~~~~~~~~~~~~~~~
//      +---> write queue ------------> ready queue --+
//      |                                             |
//      +------------------------------- freelist <---+
//                                  ~~~~~~~~~~~~~~~~~~~~~
//                                  ~  mutex protected  ~
//                                  ~~~~~~~~~~~~~~~~~~~~~
class PagePool {
 public:
  class PageBlock {
   public:
    static constexpr size_t kPagesPerBlock = 32;  // 32 * 4KB = 128 KB.
    static constexpr size_t kBlockSize = kPagesPerBlock * base::kPageSize;

    // This factory method is just that we accidentally create extra blocks
    // without realizing by triggering the default constructor in containers.
    static PageBlock Create() { return PageBlock(); }

    PageBlock(PageBlock&&) noexcept = default;
    PageBlock& operator=(PageBlock&&) = default;

    size_t size() const { return size_; }
    bool IsFull() const { return size_ >= kPagesPerBlock; }

    // Returns the pointer to the contents of the i-th page in the block.
    uint8_t* At(size_t i) const {
      PERFETTO_DCHECK(i < kPagesPerBlock);
      return reinterpret_cast<uint8_t*>(mem_.Get()) + i * base::kPageSize;
    }

    uint8_t* CurPage() const { return At(size_); }

    void NextPage() {
      PERFETTO_DCHECK(!IsFull());
      size_++;
    }

    // Releases memory of the block and marks it available for reuse.
    void Clear() {
      size_ = 0;
      mem_.AdviseDontNeed(mem_.Get(), kBlockSize);
    }

   private:
    PageBlock(const PageBlock&) = delete;
    PageBlock& operator=(const PageBlock&) = delete;
    PageBlock() { mem_ = base::PagedMemory::Allocate(kBlockSize); }

    base::PagedMemory mem_;
    size_t size_ = 0;
  };

  PagePool() {
    PERFETTO_DETACH_FROM_THREAD(writer_thread_);
    PERFETTO_DETACH_FROM_THREAD(reader_thread_);
  }

  // Grabs a new page, eventually allocating a whole new PageBlock.
  // If contents are written to the page, the caller must call EndWrite().
  // If no data is written, it is okay to leave the BeginWrite() unpaired
  // (e.g., in case of a non-blocking read returning no data) and call again
  // BeginWrite() in the future.
  uint8_t* BeginWrite() {
    PERFETTO_DCHECK_THREAD(writer_thread_);
    if (write_queue_.empty() || write_queue_.back().IsFull())
      NewPageBlock();  // Slowpath. Tries the freelist first, then allocates.
    return write_queue_.back().CurPage();
  }

  // Marks the last page as written and bumps the write pointer.
  void EndWrite() {
    PERFETTO_DCHECK_THREAD(writer_thread_);
    PERFETTO_DCHECK(!write_queue_.empty() && !write_queue_.back().IsFull());
    write_queue_.back().NextPage();
  }

  // Makes all written pages available to the reader.
  void CommitWrittenPages() {
    PERFETTO_DCHECK_THREAD(writer_thread_);
    std::lock_guard<std::mutex> lock(mutex_);
    read_queue_.insert(read_queue_.end(),
                       std::make_move_iterator(write_queue_.begin()),
                       std::make_move_iterator(write_queue_.end()));
    write_queue_.clear();
  }

  // Moves ownership of all the page blocks in the read queue to the caller.
  // The caller is expected to move them back after reading through EndRead().
  // PageBlocks will be freed if the caller doesn't call EndRead().
  std::vector<PageBlock> BeginRead() {
    PERFETTO_DCHECK_THREAD(reader_thread_);
    std::lock_guard<std::mutex> lock(mutex_);
    auto res = std::move(read_queue_);
    read_queue_.clear();
    return res;
  }

  // Returns the page blocks borrowed for read and makes them available for
  // reuse. This allows the writer to avoid doing syscalls after the initial
  // writes.
  void EndRead(std::vector<PageBlock> page_blocks);

  size_t freelist_size_for_testing() const { return freelist_.size(); }

 private:
  PagePool(const PagePool&) = delete;
  PagePool& operator=(const PagePool&) = delete;
  void NewPageBlock();

  PERFETTO_THREAD_CHECKER(writer_thread_)
  std::vector<PageBlock> write_queue_;  // Accessed exclusively by the writer.

  std::mutex mutex_;  // Protects both the read queue and the freelist.

  PERFETTO_THREAD_CHECKER(reader_thread_)
  std::vector<PageBlock> read_queue_;  // Accessed by both threads.
  std::vector<PageBlock> freelist_;    // Accessed by both threads.
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_PAGE_POOL_H_
