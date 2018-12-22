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
}

GlobalCallstackTrie::Node* GlobalCallstackTrie::Node::GetOrCreateChild(
    const Interned<Frame>& loc) {
  Node* child = children_.Get(loc);
  if (!child)
    child = children_.Emplace(loc, this);
  return child;
}

void HeapTracker::RecordMalloc(
    const std::vector<unwindstack::FrameData>& callstack,
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

  GlobalCallstackTrie::Node* node = callsites_->CreateCallsite(callstack);

  auto callstack_allocations_it = callstack_allocations_.find(node);
  if (callstack_allocations_it == callstack_allocations_.end()) {
    GlobalCallstackTrie::IncrementNode(node);
    bool inserted;
    std::tie(callstack_allocations_it, inserted) =
        callstack_allocations_.emplace(node, node);
    PERFETTO_DCHECK(inserted);
  }
  allocations_.emplace(
      address,
      Allocation(size, sequence_number, &(callstack_allocations_it->second)));

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

void HeapTracker::Dump(
    ProfilePacket::ProcessHeapSamples* proto,
    std::set<GlobalCallstackTrie::Node*>* callstacks_to_dump) {
  // There are two reasons we remove the unused callstack allocations on the
  // next iteration of Dump:
  // * We need to remove them after the callstacks were dumped, which currently
  //   happens after the allocations are dumped.
  // * This way, we do not destroy and recreate callstacks as frequently.
  for (auto it_and_alloc : dead_callstack_allocations_) {
    auto& it = it_and_alloc.first;
    uint64_t allocated = it_and_alloc.second;
    const CallstackAllocations& alloc = it->second;
    if (alloc.allocation_count == allocated && alloc.free_count == allocated)
      callstack_allocations_.erase(it);
  }
  dead_callstack_allocations_.clear();

  for (auto it = callstack_allocations_.begin();
       it != callstack_allocations_.end(); ++it) {
    const CallstackAllocations& alloc = it->second;
    callstacks_to_dump->emplace(alloc.node);
    ProfilePacket::HeapSample* sample = proto->add_samples();
    sample->set_callstack_id(alloc.node->id());
    sample->set_cumulative_allocated(alloc.allocated);
    sample->set_cumulative_freed(alloc.freed);
    sample->set_alloc_count(alloc.allocation_count);
    sample->set_free_count(alloc.free_count);

    if (alloc.allocation_count == alloc.free_count)
      dead_callstack_allocations_.emplace_back(it, alloc.allocation_count);
  }
}

uint64_t HeapTracker::GetSizeForTesting(
    const std::vector<unwindstack::FrameData>& stack) {
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
    const std::vector<unwindstack::FrameData>& callstack) {
  Node* node = &root_;
  for (const unwindstack::FrameData& loc : callstack) {
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

Interned<Frame> GlobalCallstackTrie::InternCodeLocation(
    const unwindstack::FrameData& loc) {
  Mapping map{};
  map.offset = loc.map_elf_start_offset;
  map.start = loc.map_start;
  map.end = loc.map_end;
  map.load_bias = loc.map_load_bias;
  base::StringSplitter sp(loc.map_name, '/');
  while (sp.Next())
    map.path_components.emplace_back(string_interner_.Intern(sp.cur_token()));

  Frame frame(mapping_interner_.Intern(std::move(map)),
              string_interner_.Intern(loc.function_name), loc.rel_pc);

  return frame_interner_.Intern(frame);
}

Interned<Frame> GlobalCallstackTrie::MakeRootFrame() {
  Mapping map{};

  Frame frame(mapping_interner_.Intern(std::move(map)),
              string_interner_.Intern(""), 0);

  return frame_interner_.Intern(frame);
}

void DumpState::WriteMap(ProfilePacket* packet, const Interned<Mapping> map) {
  auto map_it_and_inserted = dumped_mappings.emplace(map.id());
  if (map_it_and_inserted.second) {
    for (const Interned<std::string>& str : map->path_components)
      WriteString(packet, str);

    auto mapping = packet->add_mappings();
    mapping->set_id(map.id());
    mapping->set_offset(map->offset);
    mapping->set_start(map->start);
    mapping->set_end(map->end);
    mapping->set_load_bias(map->load_bias);
    for (const Interned<std::string>& str : map->path_components)
      mapping->add_path_string_ids(str.id());
  }
}

void DumpState::WriteFrame(ProfilePacket* packet, Interned<Frame> frame) {
  WriteMap(packet, frame->mapping);
  WriteString(packet, frame->function_name);
  bool inserted;
  std::tie(std::ignore, inserted) = dumped_frames.emplace(frame.id());
  if (inserted) {
    auto frame_proto = packet->add_frames();
    frame_proto->set_id(frame.id());
    frame_proto->set_function_name_id(frame->function_name.id());
    frame_proto->set_mapping_id(frame->mapping.id());
    frame_proto->set_rel_pc(frame->rel_pc);
  }
}

void DumpState::WriteString(ProfilePacket* packet,
                            const Interned<std::string>& str) {
  bool inserted;
  std::tie(std::ignore, inserted) = dumped_strings.emplace(str.id());
  if (inserted) {
    auto interned_string = packet->add_strings();
    interned_string->set_id(str.id());
    interned_string->set_str(str->c_str(), str->size());
  }
}

void BookkeepingThread::HandleBookkeepingRecord(BookkeepingRecord* rec) {
  BookkeepingData* bookkeeping_data = nullptr;
  if (rec->pid != 0) {
    std::lock_guard<std::mutex> l(bookkeeping_mutex_);
    auto it = bookkeeping_data_.find(rec->pid);
    if (it == bookkeeping_data_.end()) {
      PERFETTO_DFATAL("Invalid pid: %d", rec->pid);
      return;
    }
    bookkeeping_data = &it->second;
  }

  if (rec->record_type == BookkeepingRecord::Type::Dump) {
    DumpRecord& dump_rec = rec->dump_record;
    std::shared_ptr<TraceWriter> trace_writer = dump_rec.trace_writer.lock();
    if (!trace_writer)
      return;
    PERFETTO_LOG("Dumping heaps");
    std::set<GlobalCallstackTrie::Node*> callstacks_to_dump;
    TraceWriter::TracePacketHandle trace_packet =
        trace_writer->NewTracePacket();
    auto profile_packet = trace_packet->set_profile_packet();
    for (const pid_t pid : dump_rec.pids) {
      ProfilePacket::ProcessHeapSamples* sample =
          profile_packet->add_process_dumps();
      sample->set_pid(static_cast<uint64_t>(pid));
      auto it = bookkeeping_data_.find(pid);
      if (it == bookkeeping_data_.end())
        continue;

      PERFETTO_LOG("Dumping %d ", it->first);
      it->second.heap_tracker.Dump(sample, &callstacks_to_dump);
    }

    // TODO(fmayer): For incremental dumps, this should be owned by the
    // producer. This way we can keep track on what we dumped accross multiple
    // dumps.
    DumpState dump_state;

    for (GlobalCallstackTrie::Node* node : callstacks_to_dump) {
      // There need to be two separate loops over built_callstack because
      // protozero cannot interleave different messages.
      auto built_callstack = callsites_.BuildCallstack(node);
      for (const Interned<Frame>& frame : built_callstack)
        dump_state.WriteFrame(profile_packet, frame);
      ProfilePacket::Callstack* callstack = profile_packet->add_callstacks();
      callstack->set_id(node->id());
      for (const Interned<Frame>& frame : built_callstack)
        callstack->add_frame_ids(frame.id());
    }

    // We cannot garbage collect until we have finished dumping, as the state
    // in DumpState points into the GlobalCallstackTrie.
    for (const pid_t pid : dump_rec.pids) {
      auto it = bookkeeping_data_.find(pid);
      if (it == bookkeeping_data_.end())
        continue;

      if (it->second.ref_count == 0) {
        std::lock_guard<std::mutex> l(bookkeeping_mutex_);
        it = bookkeeping_data_.erase(it);
      }
    }
    trace_packet->Finalize();
    dump_rec.callback();
  } else if (rec->record_type == BookkeepingRecord::Type::Free) {
    FreeRecord& free_rec = rec->free_record;
    FreePageEntry* entries = free_rec.metadata->entries;
    uint64_t num_entries = free_rec.metadata->num_entries;
    if (num_entries > kFreePageSize)
      return;
    for (size_t i = 0; i < num_entries; ++i) {
      const FreePageEntry& entry = entries[i];
      bookkeeping_data->heap_tracker.RecordFree(entry.addr,
                                                entry.sequence_number);
    }
  } else if (rec->record_type == BookkeepingRecord::Type::Malloc) {
    AllocRecord& alloc_rec = rec->alloc_record;
    bookkeeping_data->heap_tracker.RecordMalloc(
        alloc_rec.frames, alloc_rec.alloc_metadata.alloc_address,
        alloc_rec.alloc_metadata.total_size,
        alloc_rec.alloc_metadata.sequence_number);
  } else {
    PERFETTO_DFATAL("Invalid record type");
  }
}

BookkeepingThread::ProcessHandle BookkeepingThread::NotifyProcessConnected(
    pid_t pid) {
  std::lock_guard<std::mutex> l(bookkeeping_mutex_);
  // emplace gives the existing BookkeepingData for pid if it already exists
  // or creates a new one.
  auto it_and_inserted = bookkeeping_data_.emplace(pid, &callsites_);
  BookkeepingData& bk = it_and_inserted.first->second;
  bk.ref_count++;
  return {this, pid};
}

void BookkeepingThread::NotifyProcessDisconnected(pid_t pid) {
  std::lock_guard<std::mutex> l(bookkeeping_mutex_);
  auto it = bookkeeping_data_.find(pid);
  if (it == bookkeeping_data_.end()) {
    PERFETTO_DFATAL("Client for %d not found", pid);
    return;
  }
  it->second.ref_count--;
}

void BookkeepingThread::Run(BoundedQueue<BookkeepingRecord>* input_queue) {
  for (;;) {
    BookkeepingRecord rec;
    if (!input_queue->Get(&rec))
      return;
    HandleBookkeepingRecord(&rec);
  }
}

BookkeepingThread::ProcessHandle::ProcessHandle(
    BookkeepingThread* bookkeeping_thread,
    pid_t pid)
    : bookkeeping_thread_(bookkeeping_thread), pid_(pid) {}

BookkeepingThread::ProcessHandle::~ProcessHandle() {
  if (bookkeeping_thread_)
    bookkeeping_thread_->NotifyProcessDisconnected(pid_);
}

BookkeepingThread::ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept
    : bookkeeping_thread_(other.bookkeeping_thread_), pid_(other.pid_) {
  other.bookkeeping_thread_ = nullptr;
}

BookkeepingThread::ProcessHandle& BookkeepingThread::ProcessHandle::operator=(
    ProcessHandle&& other) noexcept {
  // Construct this temporary because the RHS could be an lvalue cast to an
  // rvalue whose lifetime we do not know.
  ProcessHandle tmp(std::move(other));
  using std::swap;
  swap(*this, tmp);
  return *this;
}

void swap(BookkeepingThread::ProcessHandle& a,
          BookkeepingThread::ProcessHandle& b) {
  using std::swap;
  swap(a.bookkeeping_thread_, b.bookkeeping_thread_);
  swap(a.pid_, b.pid_);
}

}  // namespace profiling
}  // namespace perfetto
