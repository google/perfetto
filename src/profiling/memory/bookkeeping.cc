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

GlobalCallstackTrie::Node* GlobalCallstackTrie::Node::GetOrCreateChild(
    const InternedCodeLocation& loc) {
  Node* child = children_.Get(loc);
  if (!child)
    child = children_.Emplace(loc, this);
  return child;
}

std::vector<InternedCodeLocation> GlobalCallstackTrie::Node::BuildCallstack()
    const {
  const Node* node = this;
  std::vector<InternedCodeLocation> res;
  while (node) {
    res.emplace_back(node->location_);
    node = node->parent_;
  }
  return res;
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

void HeapTracker::Dump(int fd) {
  // TODO(fmayer): This should dump protocol buffers into the perfetto service.
  // For now, output a text file compatible with flamegraph.pl.
  for (const auto& p : allocations_) {
    std::string data;
    const Allocation& alloc = p.second;
    const std::vector<InternedCodeLocation> callstack =
        alloc.node->BuildCallstack();
    for (auto it = callstack.begin(); it != callstack.end(); ++it) {
      if (it != callstack.begin())
        data += ";";
      data += it->function_name.str();
    }
    data += " " + std::to_string(alloc.total_size) + "\n";
    base::WriteAll(fd, data.c_str(), data.size());
  }
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
    PERFETTO_LOG("Dumping heaps");
    auto it = bookkeeping_data_.begin();
    while (it != bookkeeping_data_.end()) {
      std::string dump_file_name = file_name_ + "." + std::to_string(it->first);
      PERFETTO_LOG("Dumping %d to %s", it->first, dump_file_name.c_str());
      base::ScopedFile fd =
          base::OpenFile(dump_file_name, O_WRONLY | O_CREAT, 0644);
      if (fd)
        it->second.heap_tracker.Dump(fd.get());
      else
        PERFETTO_PLOG("Failed to open %s", dump_file_name.c_str());
      // Garbage collect for processes that already went away.
      if (it->second.ref_count == 0) {
        std::lock_guard<std::mutex> l(bookkeeping_mutex_);
        it = bookkeeping_data_.erase(it);
      } else {
        ++it;
      }
    }
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
    std::vector<CodeLocation> code_locations;
    for (unwindstack::FrameData& frame : alloc_rec.frames)
      code_locations.emplace_back(frame.map_name, frame.function_name);
    bookkeeping_data->heap_tracker.RecordMalloc(
        code_locations, alloc_rec.alloc_metadata.alloc_address,
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
