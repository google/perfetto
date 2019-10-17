/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/db/bit_vector_iterators.h"

namespace perfetto {
namespace trace_processor {
namespace internal {

BaseIterator::BaseIterator(BitVector* bv) : bv_(bv) {
  size_ = bv->size();

  if (size_ > 0) {
    block_ = bv_->blocks_[0];
  }
}

BaseIterator::~BaseIterator() {
  uint32_t block = index_ / BitVector::Block::kBits;
  OnBlockChange(block, static_cast<uint32_t>(bv_->blocks_.size()) - 1);
}

void BaseIterator::OnBlockChange(uint32_t old_block, uint32_t new_block) {
  // If we touched the current block, flush the block to the bitvector.
  if (is_block_changed_) {
    bv_->blocks_[old_block] = block_;
  }

  if (set_bit_count_diff_ != 0) {
    // If the count of set bits has changed, go through all the counts between
    // the old and new blocks and modify them.
    // We only need to go to new_block and not to the end of the bitvector as
    // the blocks after new_block will either be updated in a future call to
    // OnBlockChange or in the destructor.
    for (uint32_t i = old_block + 1; i <= new_block; ++i) {
      int32_t new_count =
          static_cast<int32_t>(bv_->counts_[i]) + set_bit_count_diff_;
      PERFETTO_DCHECK(new_count >= 0);

      bv_->counts_[i] = static_cast<uint32_t>(new_count);
    }
  }

  // Reset the changed flag and cache the new block.
  is_block_changed_ = false;
  block_ = bv_->blocks_[new_block];
}

AllBitsIterator::AllBitsIterator(const BitVector* bv)
    : BaseIterator(const_cast<BitVector*>(bv)) {}

}  // namespace internal
}  // namespace trace_processor
}  // namespace perfetto
