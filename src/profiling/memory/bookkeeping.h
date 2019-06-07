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

#ifndef SRC_PROFILING_MEMORY_BOOKKEEPING_H_
#define SRC_PROFILING_MEMORY_BOOKKEEPING_H_

#include <map>
#include <string>
#include <vector>

#include "perfetto/ext/base/lookup_set.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/time.h"
#include "src/profiling/memory/interner.h"
#include "src/profiling/memory/unwound_messages.h"

// Below is an illustration of the bookkeeping system state where
// PID 1 does the following allocations:
// 0x123: 128 bytes at [bar main]
// 0x234: 128 bytes at [bar main]
// 0xf00: 512 bytes at [foo main]
// PID 1 allocated but previously freed 1024 bytes at [bar main]
//
// PID 2 does the following allocations:
// 0x345: 512 bytes at [foo main]
// 0x456:  32 bytes at [foo main]
// PID 2 allocated but already freed 1235 bytes at [foo main]
// PID 2 allocated and freed 2048 bytes in main.
//
// +---------------------------------+   +-------------------+
// | +---------+    HeapTracker PID 1|   | GlobalCallstackTri|
// | |0x123 128+---+    +----------+ |   |           +---+   |
// | |         |   +---->alloc:1280+----------------->bar|   |
// | |0x234 128+---+    |free: 1024| |   |           +-^-+   |
// | |         |        +----------+ |   |   +---+     ^     |
// | |0xf00 512+---+                 | +----->foo|     |     |
// | +--------+|   |    +----------+ | | |   +-^-+     |     |
// |               +---->alloc: 512+---+ |     |       |     |
// |                    |free:    0| | | |     +--+----+     |
// |                    +----------+ | | |        |          |
// |                                 | | |      +-+--+       |
// +---------------------------------+ | |      |main|       |
//                                     | |      +--+-+       |
// +---------------------------------+ | |         ^         |
// | +---------+    HeapTracker PID 2| | +-------------------+
// | |0x345 512+---+    +----------+ | |           |
// | |         |   +---->alloc:1779+---+           |
// | |0x456  32+---+    |free: 1235| |             |
// | +---------+        +----------+ |             |
// |                                 |             |
// |                    +----------+ |             |
// |                    |alloc:2048+---------------+
// |                    |free: 2048| |
// |                    +----------+ |
// |                                 |
// +---------------------------------+
//   Allocation    CallstackAllocations        Node
//
// The active allocations are on the leftmost side, modeled as the class
// HeapTracker::Allocation.
//
// The total allocated and freed bytes per callsite are in the middle, modeled
// as the HeapTracker::CallstackAllocations class.
// Note that (1280 - 1024) = 256, so alloc - free is equal to the total of the
// currently active allocations.
// Note in PID 2 there is a CallstackAllocations with 2048 allocated and 2048
// freed bytes. This is not currently referenced by any Allocations (as it
// should, as 2048 - 2048 = 0, which would mean that the total size of the
// allocations referencing it should be 0). This is because we haven't dumped
// this state yet, so the CallstackAllocations will be kept around until the
// next dump, written to the trace, and then destroyed.
//
// On the right hand side is the GlobalCallstackTrie, with nodes representing
// distinct callstacks. They have no information about the currently allocated
// or freed bytes, they only contain a reference count to destroy them as
// soon as they are no longer referenced by a HeapTracker.

namespace perfetto {
namespace profiling {

class HeapTracker;

struct Mapping {
  Mapping(Interned<std::string> b) : build_id(std::move(b)) {}

  Interned<std::string> build_id;
  uint64_t offset = 0;
  uint64_t start = 0;
  uint64_t end = 0;
  uint64_t load_bias = 0;
  std::vector<Interned<std::string>> path_components{};

  bool operator<(const Mapping& other) const {
    return std::tie(build_id, offset, start, end, load_bias, path_components) <
           std::tie(other.build_id, other.offset, other.start, other.end,
                    other.load_bias, other.path_components);
  }
  bool operator==(const Mapping& other) const {
    return std::tie(build_id, offset, start, end, load_bias, path_components) ==
           std::tie(other.build_id, other.offset, other.start, other.end,
                    other.load_bias, other.path_components);
  }
};

struct Frame {
  Frame(Interned<Mapping> m, Interned<std::string> fn_name, uint64_t pc)
      : mapping(m), function_name(fn_name), rel_pc(pc) {}
  Interned<Mapping> mapping;
  Interned<std::string> function_name;
  uint64_t rel_pc;

  bool operator<(const Frame& other) const {
    return std::tie(mapping, function_name, rel_pc) <
           std::tie(other.mapping, other.function_name, other.rel_pc);
  }

  bool operator==(const Frame& other) const {
    return std::tie(mapping, function_name, rel_pc) ==
           std::tie(other.mapping, other.function_name, other.rel_pc);
  }
};

// Graph of function callsites. This is shared between heap dumps for
// different processes. Each call site is represented by a
// GlobalCallstackTrie::Node that is owned by the parent (i.e. calling)
// callsite. It has a pointer to its parent, which means the function
// call-graph can be reconstructed from a GlobalCallstackTrie::Node by walking
// down the pointers to the parents.
class GlobalCallstackTrie {
 public:
  // Node in a tree of function traces that resulted in an allocation. For
  // instance, if alloc_buf is called from foo and bar, which are called from
  // main, the tree looks as following.
  //
  //            alloc_buf    alloc_buf
  //                   |      |
  //                  foo    bar
  //                    \    /
  //                      main
  //                       |
  //                   libc_init
  //                       |
  //                    [root_]
  //
  // allocations_ will hold a map from the pointers returned from malloc to
  // alloc_buf to the leafs of this tree.
  class Node {
   public:
    // This is opaque except to GlobalCallstackTrie.
    friend class GlobalCallstackTrie;

    Node(Interned<Frame> frame) : Node(std::move(frame), 0, nullptr) {}
    Node(Interned<Frame> frame, uint64_t id)
        : Node(std::move(frame), id, nullptr) {}
    Node(Interned<Frame> frame, uint64_t id, Node* parent)
        : id_(id), parent_(parent), location_(std::move(frame)) {}

    uint64_t id() const { return id_; }

   private:
    Node* GetOrCreateChild(const Interned<Frame>& loc);

    uint64_t ref_count_ = 0;
    uint64_t id_;
    Node* const parent_;
    const Interned<Frame> location_;
    base::LookupSet<Node, const Interned<Frame>, &Node::location_> children_;
  };

  GlobalCallstackTrie() = default;
  GlobalCallstackTrie(const GlobalCallstackTrie&) = delete;
  GlobalCallstackTrie& operator=(const GlobalCallstackTrie&) = delete;

  Node* CreateCallsite(const std::vector<FrameData>& locs);
  static void DecrementNode(Node* node);
  static void IncrementNode(Node* node);

  std::vector<Interned<Frame>> BuildCallstack(const Node* node) const;

 private:
  Node* GetOrCreateChild(Node* self, const Interned<Frame>& loc);

  Interned<Frame> InternCodeLocation(const FrameData& loc);
  Interned<Frame> MakeRootFrame();

  Interner<std::string> string_interner_;
  Interner<Mapping> mapping_interner_;
  Interner<Frame> frame_interner_;

  uint64_t next_callstack_id_ = 0;

  Node root_{MakeRootFrame(), ++next_callstack_id_};
};

// Snapshot for memory allocations of a particular process. Shares callsites
// with other processes.
class HeapTracker {
 public:
  // Sum of all the allocations for a given callstack.
  struct CallstackAllocations {
    CallstackAllocations(GlobalCallstackTrie::Node* n) : node(n) {}

    uint64_t allocs = 0;

    uint64_t allocated = 0;
    uint64_t freed = 0;
    uint64_t allocation_count = 0;
    uint64_t free_count = 0;

    GlobalCallstackTrie::Node* const node;

    ~CallstackAllocations() { GlobalCallstackTrie::DecrementNode(node); }

    bool operator<(const CallstackAllocations& other) const {
      return node < other.node;
    }
  };

  // Caller needs to ensure that callsites outlives the HeapTracker.
  explicit HeapTracker(GlobalCallstackTrie* callsites)
      : callsites_(callsites) {}

  void RecordMalloc(const std::vector<FrameData>& stack,
                    uint64_t address,
                    uint64_t size,
                    uint64_t sequence_number,
                    uint64_t timestamp);

  template <typename F>
  void GetCallstackAllocations(F fn) {
    // There are two reasons we remove the unused callstack allocations on the
    // next iteration of Dump:
    // * We need to remove them after the callstacks were dumped, which
    //   currently happens after the allocations are dumped.
    // * This way, we do not destroy and recreate callstacks as frequently.
    for (auto it_and_alloc : dead_callstack_allocations_) {
      auto& it = it_and_alloc.first;
      uint64_t allocated = it_and_alloc.second;
      const CallstackAllocations& alloc = it->second;
      if (alloc.allocs == 0 && alloc.allocation_count == allocated)
        callstack_allocations_.erase(it);
    }
    dead_callstack_allocations_.clear();

    for (auto it = callstack_allocations_.begin();
         it != callstack_allocations_.end(); ++it) {
      const CallstackAllocations& alloc = it->second;
      fn(alloc);

      if (alloc.allocs == 0)
        dead_callstack_allocations_.emplace_back(it, alloc.allocation_count);
    }
  }

  template <typename F>
  void GetAllocations(F fn) {
    for (const auto& addr_and_allocation : allocations_) {
      const Allocation& alloc = addr_and_allocation.second;
      fn(addr_and_allocation.first, alloc.total_size,
         alloc.callstack_allocations->node->id());
    }
  }

  void RecordFree(uint64_t address,
                  uint64_t sequence_number,
                  uint64_t timestamp) {
    RecordOperation(sequence_number, {address, timestamp});
  }

  uint64_t committed_timestamp() { return committed_timestamp_; }

  uint64_t GetSizeForTesting(const std::vector<FrameData>& stack);
  uint64_t GetTimestampForTesting() { return committed_timestamp_; }

 private:
  struct Allocation {
    Allocation(uint64_t size, uint64_t seq, CallstackAllocations* csa)
        : total_size(size), sequence_number(seq), callstack_allocations(csa) {
      callstack_allocations->allocs++;
    }

    Allocation() = default;
    Allocation(const Allocation&) = delete;
    Allocation(Allocation&& other) noexcept {
      total_size = other.total_size;
      sequence_number = other.sequence_number;
      callstack_allocations = other.callstack_allocations;
      other.callstack_allocations = nullptr;
    }

    void AddToCallstackAllocations() {
      callstack_allocations->allocation_count++;
      callstack_allocations->allocated += total_size;
    }

    void SubtractFromCallstackAllocations() {
      callstack_allocations->free_count++;
      callstack_allocations->freed += total_size;
    }

    ~Allocation() {
      if (callstack_allocations)
        callstack_allocations->allocs--;
    }

    uint64_t total_size;
    uint64_t sequence_number;
    CallstackAllocations* callstack_allocations;
  };

  struct PendingOperation {
    uint64_t allocation_address;
    uint64_t timestamp;
  };

  CallstackAllocations* MaybeCreateCallstackAllocations(
      GlobalCallstackTrie::Node* node) {
    auto callstack_allocations_it = callstack_allocations_.find(node);
    if (callstack_allocations_it == callstack_allocations_.end()) {
      GlobalCallstackTrie::IncrementNode(node);
      bool inserted;
      std::tie(callstack_allocations_it, inserted) =
          callstack_allocations_.emplace(node, node);
      PERFETTO_DCHECK(inserted);
    }
    return &callstack_allocations_it->second;
  }

  void RecordOperation(uint64_t sequence_number,
                       const PendingOperation& operation);

  // Commits a malloc or free operation.
  // See comment of pending_operations_ for encoding of malloc and free
  // operations.
  //
  // Committing a malloc operation: Add the allocations size to
  // CallstackAllocation::allocated.
  // Committing a free operation: Add the allocation's size to
  // CallstackAllocation::freed and delete the allocation.
  void CommitOperation(uint64_t sequence_number,
                       const PendingOperation& operation);

  // We cannot use an interner here, because after the last allocation goes
  // away, we still need to keep the CallstackAllocations around until the next
  // dump.
  std::map<GlobalCallstackTrie::Node*, CallstackAllocations>
      callstack_allocations_;

  std::vector<std::pair<decltype(callstack_allocations_)::iterator, uint64_t>>
      dead_callstack_allocations_;

  std::map<uint64_t /* allocation address */, Allocation> allocations_;

  // An operation is either a commit of an allocation or freeing of an
  // allocation. An operation is a free if its seq_id is larger than
  // the sequence_number of the corresponding allocation. It is a commit if its
  // seq_id is equal to the sequence_number of the corresponding allocation.
  //
  // If its seq_id is less than the sequence_number of the corresponding
  // allocation it could be either, but is ignored either way.
  std::map<uint64_t /* seq_id */, PendingOperation /* allocation address */>
      pending_operations_;

  uint64_t committed_timestamp_ = 0;
  // The sequence number all mallocs and frees have been handled up to.
  uint64_t committed_sequence_number_ = 0;
  GlobalCallstackTrie* callsites_;
};

}  // namespace profiling
}  // namespace perfetto

namespace std {
template <>
struct hash<::perfetto::profiling::Mapping> {
  using argument_type = ::perfetto::profiling::Mapping;
  using result_type = size_t;
  result_type operator()(const argument_type& mapping) {
    size_t h =
        std::hash<::perfetto::profiling::InternID>{}(mapping.build_id.id());
    h ^= std::hash<uint64_t>{}(mapping.offset);
    h ^= std::hash<uint64_t>{}(mapping.start);
    h ^= std::hash<uint64_t>{}(mapping.end);
    h ^= std::hash<uint64_t>{}(mapping.load_bias);
    for (const auto& path : mapping.path_components)
      h ^= std::hash<uint64_t>{}(path.id());
    return h;
  }
};

template <>
struct hash<::perfetto::profiling::Frame> {
  using argument_type = ::perfetto::profiling::Frame;
  using result_type = size_t;
  result_type operator()(const argument_type& frame) {
    size_t h = std::hash<::perfetto::profiling::InternID>{}(frame.mapping.id());
    h ^= std::hash<::perfetto::profiling::InternID>{}(frame.function_name.id());
    h ^= std::hash<uint64_t>{}(frame.rel_pc);
    return h;
  }
};
}  // namespace std

#endif  // SRC_PROFILING_MEMORY_BOOKKEEPING_H_
