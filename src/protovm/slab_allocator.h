/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_PROTOVM_SLAB_ALLOCATOR_H_
#define SRC_PROTOVM_SLAB_ALLOCATOR_H_

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace perfetto {
namespace protovm {

// An efficient allocator for elements with fixed size and alignment
// requirements.
//
// Key features:
//
// - Slab allocation: instead of requesting memory for each individual element,
//   it allocates large chunks of memory (slabs) upfront, where each slab can
//   hold multiple elements.
//
// - Free list management: a free list keeps track of available elements within
//   the allocated slabs. When a request for allocation comes in, the allocator
//   simply takes an element from the free list. Deallocation returns the
//   element back to the free list.
template <size_t ElementSize, size_t ElementAlign, size_t SlabCapacity = 64>
class SlabAllocator {
 public:
  explicit SlabAllocator() : next_free_slot_{nullptr} {}

  void* Allocate() {
    if (!next_free_slot_) {
      auto slab = CreateSlab();
      if (!slab) {
        return nullptr;
      }
      slabs_.push_back(std::move(slab));
      next_free_slot_ = &slabs_.back()[0];
    }

    auto* slot = next_free_slot_;
    next_free_slot_ = slot->next;
    memset(&slot->element, 0, ElementSize);
    return &slot->element;
  }

  void Free(void* p) {
    auto* slot = static_cast<Slot*>(p);
    slot->next = next_free_slot_;
    next_free_slot_ = slot;
  }

 private:
  union Slot {
    Slot* next;
    alignas(ElementAlign) unsigned char element[ElementSize];
  };

  std::unique_ptr<Slot[]> CreateSlab() {
    auto slab = std::unique_ptr<Slot[]>(new Slot[SlabCapacity]);

    for (size_t i = 0; i + 1 < SlabCapacity; ++i) {
      auto& slot = slab[i];
      auto& next_slot = slab[i + 1];
      slot.next = &next_slot;
    }

    auto& last_slot = slab[SlabCapacity - 1];
    last_slot.next = nullptr;

    return slab;
  }

  Slot* next_free_slot_;
  std::vector<std::unique_ptr<Slot[]>> slabs_;
};

}  // namespace protovm
}  // namespace perfetto

#endif  // SRC_PROTOVM_SLAB_ALLOCATOR_H_
