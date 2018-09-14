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

#include "src/profiling/memory/bookkeeping.h"

namespace perfetto {

MemoryBookkeeping::Node* MemoryBookkeeping::Node::GetOrCreateChild(
    const MemoryBookkeeping::InternedCodeLocation& loc) {
  Node* child = children_.Get(loc);
  if (!child)
    child = children_.Emplace(loc, this);
  return child;
}

void MemoryBookkeeping::RecordMalloc(const std::vector<CodeLocation>& locs,
                                     uint64_t address,
                                     uint64_t size) {
  Node* node = &root_;
  node->cum_size_ += size;
  for (const MemoryBookkeeping::CodeLocation& loc : locs) {
    node = node->GetOrCreateChild(InternCodeLocation(loc));
    node->cum_size_ += size;
  }

  allocations_.emplace(address, std::make_pair(size, node));
}

void MemoryBookkeeping::RecordFree(uint64_t address) {
  auto leaf_it = allocations_.find(address);
  if (leaf_it == allocations_.end())
    return;

  std::pair<uint64_t, Node*> value = leaf_it->second;
  uint64_t size = value.first;
  Node* node = value.second;

  bool delete_prev = false;
  Node* prev = nullptr;
  while (node != nullptr) {
    if (delete_prev)
      node->children_.Remove(*prev);
    node->cum_size_ -= size;
    delete_prev = node->cum_size_ == 0;
    prev = node;
    node = node->parent_;
  }

  allocations_.erase(leaf_it);
}

uint64_t MemoryBookkeeping::GetCumSizeForTesting(
    const std::vector<CodeLocation>& locs) {
  Node* node = &root_;
  for (const MemoryBookkeeping::CodeLocation& loc : locs) {
    node = node->children_.Get(InternCodeLocation(loc));
    if (node == nullptr)
      return 0;
  }
  return node->cum_size_;
}

}  // namespace perfetto
