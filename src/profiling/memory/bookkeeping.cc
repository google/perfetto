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

#include "perfetto/base/file_utils.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/scoped_file.h"

namespace perfetto {
namespace profiling {
namespace {
using ::perfetto::protos::pbzero::ProfilePacket;
// This needs to be lower than the maximum acceptable chunk size, because this
// is checked *before* writing another submessage. We conservatively assume
// submessages can be up to 100k here for a 500k chunk size.
// DropBox has a 500k chunk limit, and each chunk needs to parse as a proto.
uint32_t kPacketSizeThreshold = 400000;
}

GlobalCallstackTrie::Node* GlobalCallstackTrie::Node::GetOrCreateChild(
    const Interned<Frame>& loc) {
  Node* child = children_.Get(loc);
  if (!child)
    child = children_.Emplace(loc, this);
  return child;
}

void HeapTracker::RecordMalloc(const std::vector<FrameData>& callstack,
                               uint64_t address,
                               uint64_t size,
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
      alloc.total_size = size;
      alloc.sequence_number = sequence_number;
      alloc.SetCallstackAllocations(MaybeCreateCallstackAllocations(node));
    }
  } else {
    GlobalCallstackTrie::Node* node = callsites_->CreateCallsite(callstack);
    allocations_.emplace(address,
                         Allocation(size, sequence_number,
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

void HeapTracker::Dump(
    std::function<void(ProfilePacket::ProcessHeapSamples*)> fill_process_header,
    DumpState* dump_state) {
  // There are two reasons we remove the unused callstack allocations on the
  // next iteration of Dump:
  // * We need to remove them after the callstacks were dumped, which currently
  //   happens after the allocations are dumped.
  // * This way, we do not destroy and recreate callstacks as frequently.
  for (auto it_and_alloc : dead_callstack_allocations_) {
    auto& it = it_and_alloc.first;
    uint64_t allocated = it_and_alloc.second;
    const CallstackAllocations& alloc = it->second;
    if (alloc.allocs == 0 && alloc.allocation_count == allocated)
      callstack_allocations_.erase(it);
  }
  dead_callstack_allocations_.clear();

  if (dump_state->currently_written() > kPacketSizeThreshold)
    dump_state->NewProfilePacket();

  ProfilePacket::ProcessHeapSamples* proto =
      dump_state->current_profile_packet->add_process_dumps();
  fill_process_header(proto);
  proto->set_timestamp(committed_timestamp_);
  for (auto it = callstack_allocations_.begin();
       it != callstack_allocations_.end(); ++it) {
    if (dump_state->currently_written() > kPacketSizeThreshold) {
      dump_state->NewProfilePacket();
      proto = dump_state->current_profile_packet->add_process_dumps();
      fill_process_header(proto);
      proto->set_timestamp(committed_timestamp_);
    }

    const CallstackAllocations& alloc = it->second;
    dump_state->callstacks_to_dump.emplace(alloc.node);
    ProfilePacket::HeapSample* sample = proto->add_samples();
    sample->set_callstack_id(alloc.node->id());
    sample->set_self_allocated(alloc.allocated);
    sample->set_self_freed(alloc.freed);
    sample->set_alloc_count(alloc.allocation_count);
    sample->set_free_count(alloc.free_count);

    if (alloc.allocs == 0)
      dead_callstack_allocations_.emplace_back(it, alloc.allocation_count);
  }
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
    node = node->GetOrCreateChild(InternCodeLocation(loc));
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

void DumpState::WriteMap(const Interned<Mapping> map) {
  auto map_it_and_inserted = dumped_mappings.emplace(map.id());
  if (map_it_and_inserted.second) {
    for (const Interned<std::string>& str : map->path_components)
      WriteString(str);

    WriteString(map->build_id);

    if (currently_written() > kPacketSizeThreshold)
      NewProfilePacket();

    auto mapping = current_profile_packet->add_mappings();
    mapping->set_id(map.id());
    mapping->set_offset(map->offset);
    mapping->set_start(map->start);
    mapping->set_end(map->end);
    mapping->set_load_bias(map->load_bias);
    mapping->set_build_id(map->build_id.id());
    for (const Interned<std::string>& str : map->path_components)
      mapping->add_path_string_ids(str.id());
  }
}

void DumpState::WriteFrame(Interned<Frame> frame) {
  WriteMap(frame->mapping);
  WriteString(frame->function_name);
  bool inserted;
  std::tie(std::ignore, inserted) = dumped_frames.emplace(frame.id());
  if (inserted) {
    if (currently_written() > kPacketSizeThreshold)
      NewProfilePacket();

    auto frame_proto = current_profile_packet->add_frames();
    frame_proto->set_id(frame.id());
    frame_proto->set_function_name_id(frame->function_name.id());
    frame_proto->set_mapping_id(frame->mapping.id());
    frame_proto->set_rel_pc(frame->rel_pc);
  }
}

void DumpState::WriteString(const Interned<std::string>& str) {
  bool inserted;
  std::tie(std::ignore, inserted) = dumped_strings.emplace(str.id());
  if (inserted) {
    if (currently_written() > kPacketSizeThreshold)
      NewProfilePacket();

    auto interned_string = current_profile_packet->add_strings();
    interned_string->set_id(str.id());
    interned_string->set_str(reinterpret_cast<const uint8_t*>(str->c_str()),
                             str->size());
  }
}

}  // namespace profiling
}  // namespace perfetto
