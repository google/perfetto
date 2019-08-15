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

#include "src/trace_processor/string_pool.h"

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

StringPool::StringPool() {
  blocks_.emplace_back(kDefaultBlockSize);

  // Reserve a slot for the null string.
  PERFETTO_CHECK(blocks_.back().TryInsert(NullTermStringView()));
}

StringPool::~StringPool() = default;

StringPool::StringPool(StringPool&&) noexcept = default;
StringPool& StringPool::operator=(StringPool&&) = default;

StringPool::Id StringPool::InsertString(base::StringView str, uint64_t hash) {
  // Try and find enough space in the current block for the string and the
  // metadata (varint-encoded size + the string data + the null terminator).
  const uint8_t* ptr = blocks_.back().TryInsert(str);
  if (PERFETTO_UNLIKELY(!ptr)) {
    // This means the block did not have enough space. This should only happen
    // on 32-bit platforms as we allocate a 4GB mmap on 64 bit.
    PERFETTO_CHECK(sizeof(uint8_t*) == 4);

    // Add a new block to store the data. If the string is larger that the
    // default block size, add a bigger block exlusively for this string.
    if (str.size() + kMaxMetadataSize > kDefaultBlockSize) {
      blocks_.emplace_back(str.size() + kMaxMetadataSize);
    } else {
      blocks_.emplace_back(kDefaultBlockSize);
    }

    // Try and reserve space again - this time we should definitely succeed.
    ptr = blocks_.back().TryInsert(str);
    PERFETTO_CHECK(ptr);
  }

  // Finish by computing the id of the pointer and adding a mapping from the
  // hash to the string_id.
  Id string_id = PtrToId(ptr);
  string_index_.emplace(hash, string_id);
  return string_id;
}

const uint8_t* StringPool::Block::TryInsert(base::StringView str) {
  auto str_size = str.size();
  if (static_cast<uint64_t>(pos_) + str_size + kMaxMetadataSize > size_)
    return nullptr;

  // Get where we should start writing this string.
  uint8_t* begin = Get(pos_);

  // First write the size of the string using varint encoding.
  uint8_t* end = protozero::proto_utils::WriteVarInt(str_size, begin);

  // Next the string itself.
  if (PERFETTO_LIKELY(str_size > 0)) {
    memcpy(end, str.data(), str_size);
    end += str_size;
  }

  // Finally add a null terminator.
  *(end++) = '\0';

  // Update the end of the block and return the pointer to the string.
  pos_ = OffsetOf(end);
  return begin;
}

StringPool::Iterator::Iterator(const StringPool* pool) : pool_(pool) {}

StringPool::Iterator& StringPool::Iterator::operator++() {
  PERFETTO_DCHECK(block_id_ < pool_->blocks_.size());

  // Try and go to the next string in the current block.
  const auto& block = pool_->blocks_[block_id_];

  // Find the size of the string at the current offset in the block
  // and increment the offset by that size.
  uint32_t str_size = 0;
  const uint8_t* ptr = block.Get(block_offset_);
  ptr = ReadSize(ptr, &str_size);
  ptr += str_size + 1;
  block_offset_ = block.OffsetOf(ptr);

  // If we're out of bounds for this block, go to the start of the next block.
  if (block.pos() <= block_offset_) {
    block_id_++;
    block_offset_ = 0;
  }
  return *this;
}

StringPool::Iterator::operator bool() const {
  return block_id_ < pool_->blocks_.size();
}

NullTermStringView StringPool::Iterator::StringView() {
  PERFETTO_DCHECK(block_id_ < pool_->blocks_.size());
  PERFETTO_DCHECK(block_offset_ < pool_->blocks_[block_id_].pos());

  // If we're at (0, 0), we have the null string.
  if (block_id_ == 0 && block_offset_ == 0)
    return NullTermStringView();
  return GetFromPtr(pool_->blocks_[block_id_].Get(block_offset_));
}

StringPool::Id StringPool::Iterator::StringId() {
  PERFETTO_DCHECK(block_id_ < pool_->blocks_.size());
  PERFETTO_DCHECK(block_offset_ < pool_->blocks_[block_id_].pos());

  // If we're at (0, 0), we have the null string which has id 0.
  if (block_id_ == 0 && block_offset_ == 0)
    return 0;
  return pool_->PtrToId(pool_->blocks_[block_id_].Get(block_offset_));
}

}  // namespace trace_processor
}  // namespace perfetto
