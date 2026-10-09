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

#include "perfetto/ext/base/flat_hash_map.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"

namespace perfetto::base::flat_hash_map_v2_internal {

void FlatHashMapV2Base::Reset(size_t n,
                              bool reallocate,
                              size_t slot_size,
                              size_t slot_align) {
  // Must be a pow2.
  PERFETTO_CHECK((n & (n - 1)) == 0);

  // Always ensure at least 128 capacity to avoid too frequent growths.
  capacity_ = std::max<size_t>(n, 128u);
  size_ = 0;
  growth_info_.growth_left =
      (capacity_ * static_cast<size_t>(load_limit_percent_)) / 100;
  growth_info_.has_tombstones = 0;

  if (reallocate) {
    // See memory layout comment above |storage_| in flat_hash_map.h.
    size_t slots_offset = base::AlignUp(capacity_ + kNumClones, slot_align);
    storage_.reset(new uint8_t[slots_offset + (capacity_ * slot_size)]);
    ctrl_ = storage_.get();
    slots_ = storage_.get() + slots_offset;
  }
  if (ctrl_) {
    // Initialize all control bytes (including clones) to empty (kFreeSlot)
    memset(ctrl_, kFreeSlot, capacity_ + kNumClones);
  }
}

size_t FlatHashMapV2Base::FindFirstEmptyOrTombstone(size_t key_hash) const {
  return FindFirstEmptyOrTombstoneImpl(key_hash);
}

FlatHashMapV2Base::OldTable FlatHashMapV2Base::Grow(size_t slot_size,
                                                    size_t slot_align) {
  PERFETTO_DCHECK(size_ <= capacity_);
  OldTable old{std::move(storage_), ctrl_, slots_, capacity_, size_};
  // This must be a CHECK (i.e. not just a DCHECK) to prevent UAF attacks on
  // 32-bit archs that try to double the size of the table until wrapping.
  size_t new_capacity = capacity_ * 2;
  PERFETTO_CHECK(new_capacity >= capacity_);
  Reset(new_capacity, true, slot_size, slot_align);
  return old;
}

}  // namespace perfetto::base::flat_hash_map_v2_internal
