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

#include "perfetto/base/logging.h"

namespace perfetto {

GlobalCallstackTrie::Node* GlobalCallstackTrie::Node::GetOrCreateChild(
    const InternedCodeLocation& loc) {
  Node* child = children_.Get(loc);
  if (!child)
    child = children_.Emplace(loc, this);
  return child;
}

void HeapTracker::RecordMalloc(const std::vector<CodeLocation>& callstack,
                               uint64_t address,
                               uint64_t size,
                               uint64_t sequence_number) {
  auto it = allocations_.find(address);
  if (it != allocations_.end()) {
    if (it->second.sequence_number > sequence_number) {
      return;
    } else {
      // Clean up previous allocation by pretending a free happened just after
      // it.
      // CommitFree only uses the sequence number to check whether the
      // currently active allocation is newer than the free, so we can make
      // up a sequence_number here.
      CommitFree(it->second.sequence_number + 1, address);
    }
  }

  GlobalCallstackTrie::Node* node =
      callsites_->IncrementCallsite(callstack, size);
  allocations_.emplace(address, Allocation(size, sequence_number, node));

  // Keep the sequence tracker consistent.
  RecordFree(kNoopFree, sequence_number);
}

void HeapTracker::RecordFree(uint64_t address, uint64_t sequence_number) {
  if (sequence_number != sequence_number_ + 1) {
    pending_frees_.emplace(sequence_number, address);
    return;
  }

  if (address != kNoopFree)
    CommitFree(sequence_number, address);
  sequence_number_++;

  // At this point some other pending frees might be eligible to be committed.
  auto it = pending_frees_.begin();
  while (it != pending_frees_.end() && it->first == sequence_number_ + 1) {
    if (it->second != kNoopFree)
      CommitFree(it->first, it->second);
    sequence_number_++;
    it = pending_frees_.erase(it);
  }
}

void HeapTracker::CommitFree(uint64_t sequence_number, uint64_t address) {
  auto leaf_it = allocations_.find(address);
  if (leaf_it == allocations_.end())
    return;

  const Allocation& value = leaf_it->second;
  if (value.sequence_number > sequence_number)
    return;
  allocations_.erase(leaf_it);
}

uint64_t GlobalCallstackTrie::GetCumSizeForTesting(
    const std::vector<CodeLocation>& callstack) {
  Node* node = &root_;
  for (const CodeLocation& loc : callstack) {
    node = node->children_.Get(InternCodeLocation(loc));
    if (node == nullptr)
      return 0;
  }
  return node->cum_size_;
}

GlobalCallstackTrie::Node* GlobalCallstackTrie::IncrementCallsite(
    const std::vector<CodeLocation>& callstack,
    uint64_t size) {
  Node* node = &root_;
  node->cum_size_ += size;
  for (const CodeLocation& loc : callstack) {
    node = node->GetOrCreateChild(InternCodeLocation(loc));
    node->cum_size_ += size;
  }
  return node;
}

void GlobalCallstackTrie::DecrementNode(Node* node, uint64_t size) {
  PERFETTO_DCHECK(node->cum_size_ >= size);

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
}

}  // namespace perfetto
