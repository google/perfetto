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
    const Interner<Frame>::Interned& loc) {
  Node* child = children_.Get(loc);
  if (!child)
    child = children_.Emplace(loc, this);
  return child;
}

std::vector<Interner<Frame>::Interned>
GlobalCallstackTrie::Node::BuildCallstack() const {
  const Node* node = this;
  std::vector<Interner<Frame>::Interned> res;
  while (node) {
    res.emplace_back(node->location_);
    node = node->parent_;
  }
  return res;
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

void HeapTracker::Dump(
    ProfilePacket::ProcessHeapSamples* proto,
    std::set<GlobalCallstackTrie::Node*>* callstacks_to_dump) {
  for (const auto& p : allocations_) {
    const Allocation& alloc = p.second;
    callstacks_to_dump->emplace(alloc.node);
    ProfilePacket::HeapSample* sample = proto->add_samples();
    sample->set_callstack_id(alloc.node->id());
    sample->set_cumulative_allocated(alloc.total_size);
  }
}

uint64_t GlobalCallstackTrie::GetCumSizeForTesting(
    const std::vector<unwindstack::FrameData>& callstack) {
  Node* node = &root_;
  for (const unwindstack::FrameData& loc : callstack) {
    node = node->children_.Get(InternCodeLocation(loc));
    if (node == nullptr)
      return 0;
  }
  return node->cum_size_;
}

GlobalCallstackTrie::Node* GlobalCallstackTrie::IncrementCallsite(
    const std::vector<unwindstack::FrameData>& callstack,
    uint64_t size) {
  Node* node = &root_;
  node->cum_size_ += size;
  for (const unwindstack::FrameData& loc : callstack) {
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

Interner<Frame>::Interned GlobalCallstackTrie::InternCodeLocation(
    const unwindstack::FrameData& loc) {
  Mapping map{};
  map.offset = loc.map_offset;
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

Interner<Frame>::Interned GlobalCallstackTrie::MakeRootFrame() {
  Mapping map{};

  Frame frame(mapping_interner_.Intern(std::move(map)),
              string_interner_.Intern(""), 0);

  return frame_interner_.Intern(frame);
}

void DumpState::WriteMap(ProfilePacket* packet,
                         const Interner<Mapping>::Interned map) {
  auto map_it_and_inserted = dumped_mappings.emplace(map.id());
  if (map_it_and_inserted.second) {
    for (const Interner<std::string>::Interned& str : map->path_components)
      WriteString(packet, str);

    auto mapping = packet->add_mappings();
    mapping->set_offset(map->offset);
    mapping->set_start(map->start);
    mapping->set_end(map->end);
    mapping->set_load_bias(map->load_bias);
    for (const Interner<std::string>::Interned& str : map->path_components)
      mapping->add_path_string_ids(str.id());
  }
}

void DumpState::WriteFrame(ProfilePacket* packet,
                           Interner<Frame>::Interned frame) {
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
                            const Interner<std::string>::Interned& str) {
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
      auto built_callstack = node->BuildCallstack();
      for (const Interner<Frame>::Interned& frame : built_callstack)
        dump_state.WriteFrame(profile_packet, frame);
      ProfilePacket::Callstack* callstack = profile_packet->add_callstacks();
      callstack->set_id(node->id());
      for (const Interner<Frame>::Interned& frame : built_callstack)
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

void BookkeepingThread::NotifyClientConnected(pid_t pid) {
  std::lock_guard<std::mutex> l(bookkeeping_mutex_);
  // emplace gives the existing BookkeepingData for pid if it already exists
  // or creates a new one.
  auto it_and_inserted = bookkeeping_data_.emplace(pid, &callsites_);
  BookkeepingData& bk = it_and_inserted.first->second;
  bk.ref_count++;
}

void BookkeepingThread::NotifyClientDisconnected(pid_t pid) {
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

}  // namespace profiling
}  // namespace perfetto
