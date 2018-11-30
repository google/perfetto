/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/traced/probes/ftrace/page_pool.h"

#include <array>

namespace perfetto {

namespace {
constexpr size_t kMaxFreelistBlocks = 128;  // 128 * 32 * 4KB = 16MB.
}

void PagePool::NewPageBlock() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (freelist_.empty()) {
    write_queue_.emplace_back(PageBlock::Create());
  } else {
    write_queue_.emplace_back(std::move(freelist_.back()));
    freelist_.pop_back();
  }
  PERFETTO_DCHECK(write_queue_.back().size() == 0);
}

void PagePool::EndRead(std::vector<PageBlock> page_blocks) {
  PERFETTO_DCHECK_THREAD(reader_thread_);
  for (PageBlock& page_block : page_blocks)
    page_block.Clear();

  std::lock_guard<std::mutex> lock(mutex_);
  freelist_.insert(freelist_.end(),
                   std::make_move_iterator(page_blocks.begin()),
                   std::make_move_iterator(page_blocks.end()));

  // Even if blocks in the freelist don't waste any resident memory (because
  // the Clear() call above madvise()s them) let's avoid that in pathological
  // cases we keep accumulating virtual address space reservations.
  if (freelist_.size() > kMaxFreelistBlocks)
    freelist_.erase(freelist_.begin() + kMaxFreelistBlocks, freelist_.end());
}

}  // namespace perfetto
