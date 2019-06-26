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

#include <fcntl.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"

namespace perfetto {
namespace profiling {

void HeapTracker::RecordMalloc(const std::vector<FrameData>& callstack,
                               uint64_t address,
                               uint64_t sample_size,
                               uint64_t alloc_size,
                               uint64_t sequence_number,
                               uint64_t timestamp) {
  auto it = allocations_.find(address);
  if (it != allocations_.end()) {
    Allocation& alloc = it->second;
    PERFETTO_DCHECK(alloc.sequence_number != sequence_number);
    if (alloc.sequence_number < sequence_number) {
      // As we are overwriting the previous allocation, the previous allocation
      // must have been freed.
      //
      // This makes the sequencing a bit incorrect. We are overwriting this
      // allocation, so we prentend both the alloc and the free for this have
      // already happened at committed_sequence_number_, while in fact the free
      // might not have happened until right before this operation.

      if (alloc.sequence_number > committed_sequence_number_) {
        // Only count the previous allocation if it hasn't already been
        // committed to avoid double counting it.
        alloc.AddToCallstackAllocations();
      }

      alloc.SubtractFromCallstackAllocations();
      GlobalCallstackTrie::Node* node = callsites_->CreateCallsite(callstack);
      alloc.sample_size = sample_size;
      alloc.alloc_size = alloc_size;
      alloc.sequence_number = sequence_number;
      alloc.callstack_allocations = MaybeCreateCallstackAllocations(node);
    }
  } else {
    GlobalCallstackTrie::Node* node = callsites_->CreateCallsite(callstack);
    allocations_.emplace(address,
                         Allocation(sample_size, alloc_size, sequence_number,
                                    MaybeCreateCallstackAllocations(node)));
  }

  RecordOperation(sequence_number, {address, timestamp});
}

void HeapTracker::RecordOperation(uint64_t sequence_number,
                                  const PendingOperation& operation) {
  if (sequence_number != committed_sequence_number_ + 1) {
    pending_operations_.emplace(sequence_number, operation);
    return;
  }

  CommitOperation(sequence_number, operation);

  // At this point some other pending operations might be eligible to be
  // committed.
  auto it = pending_operations_.begin();
  while (it != pending_operations_.end() &&
         it->first == committed_sequence_number_ + 1) {
    CommitOperation(it->first, it->second);
    it = pending_operations_.erase(it);
  }
}

void HeapTracker::CommitOperation(uint64_t sequence_number,
                                  const PendingOperation& operation) {
  committed_sequence_number_++;
  committed_timestamp_ = operation.timestamp;

  uint64_t address = operation.allocation_address;

  // We will see many frees for addresses we do not know about.
  auto leaf_it = allocations_.find(address);
  if (leaf_it == allocations_.end())
    return;

  Allocation& value = leaf_it->second;
  if (value.sequence_number == sequence_number) {
    value.AddToCallstackAllocations();
  } else if (value.sequence_number < sequence_number) {
    value.SubtractFromCallstackAllocations();
    allocations_.erase(leaf_it);
  }
  // else (value.sequence_number > sequence_number:
  //  This allocation has been replaced by a newer one in RecordMalloc.
  //  This code commits ther previous allocation's malloc (and implicit free
  //  that must have happened, as there is now a new allocation at the same
  //  address). This means that this operation, be it a malloc or a free, must
  //  be treated as a no-op.
}

uint64_t HeapTracker::GetSizeForTesting(const std::vector<FrameData>& stack) {
  GlobalCallstackTrie::Node* node = callsites_->CreateCallsite(stack);
  // Hack to make it go away again if it wasn't used before.
  // This is only good because this is used for testing only.
  GlobalCallstackTrie::IncrementNode(node);
  GlobalCallstackTrie::DecrementNode(node);
  auto it = callstack_allocations_.find(node);
  if (it == callstack_allocations_.end()) {
    return 0;
  }
  const CallstackAllocations& alloc = it->second;
  return alloc.allocated - alloc.freed;
}

GlobalCallstackTrie::Node* GlobalCallstackTrie::GetOrCreateChild(
    Node* self,
    const Interned<Frame>& loc) {
  Node* child = self->children_.Get(loc);
  if (!child)
    child = self->children_.Emplace(loc, ++next_callstack_id_, self);
  return child;
}

std::vector<Interned<Frame>> GlobalCallstackTrie::BuildCallstack(
    const Node* node) const {
  std::vector<Interned<Frame>> res;
  while (node != &root_) {
    res.emplace_back(node->location_);
    node = node->parent_;
  }
  return res;
}

GlobalCallstackTrie::Node* GlobalCallstackTrie::CreateCallsite(
    const std::vector<FrameData>& callstack) {
  Node* node = &root_;
  for (const FrameData& loc : callstack) {
    node = GetOrCreateChild(node, InternCodeLocation(loc));
  }
  return node;
}

void GlobalCallstackTrie::IncrementNode(Node* node) {
  while (node != nullptr) {
    node->ref_count_ += 1;
    node = node->parent_;
  }
}

void GlobalCallstackTrie::DecrementNode(Node* node) {
  PERFETTO_DCHECK(node->ref_count_ >= 1);

  bool delete_prev = false;
  Node* prev = nullptr;
  while (node != nullptr) {
    if (delete_prev)
      node->children_.Remove(*prev);
    node->ref_count_ -= 1;
    delete_prev = node->ref_count_ == 0;
    prev = node;
    node = node->parent_;
  }
}

Interned<Frame> GlobalCallstackTrie::InternCodeLocation(const FrameData& loc) {
  Mapping map(string_interner_.Intern(loc.build_id));
  map.offset = loc.frame.map_elf_start_offset;
  map.start = loc.frame.map_start;
  map.end = loc.frame.map_end;
  map.load_bias = loc.frame.map_load_bias;
  base::StringSplitter sp(loc.frame.map_name, '/');
  while (sp.Next())
    map.path_components.emplace_back(string_interner_.Intern(sp.cur_token()));

  Frame frame(mapping_interner_.Intern(std::move(map)),
              string_interner_.Intern(loc.frame.function_name),
              loc.frame.rel_pc);

  return frame_interner_.Intern(frame);
}

Interned<Frame> GlobalCallstackTrie::MakeRootFrame() {
  Mapping map(string_interner_.Intern(""));

  Frame frame(mapping_interner_.Intern(std::move(map)),
              string_interner_.Intern(""), 0);

  return frame_interner_.Intern(frame);
}

}  // namespace profiling
}  // namespace perfetto
