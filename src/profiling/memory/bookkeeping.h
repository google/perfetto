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

#include "perfetto/base/lookup_set.h"
#include "perfetto/base/string_splitter.h"
#include "perfetto/trace/profiling/profile_packet.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "src/profiling/memory/bounded_queue.h"
#include "src/profiling/memory/interner.h"
#include "src/profiling/memory/queue_messages.h"

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
  uint64_t build_id;
  uint64_t offset;
  uint64_t start;
  uint64_t end;
  uint64_t load_bias;
  std::vector<Interner<std::string>::Interned> path_components;

  bool operator<(const Mapping& other) const {
    return std::tie(build_id, offset, start, end, load_bias, path_components) <
           std::tie(other.build_id, other.offset, other.start, other.end,
                    other.load_bias, other.path_components);
  }
};

struct Frame {
  Frame(Interner<Mapping>::Interned m,
        Interner<std::string>::Interned fn_name,
        uint64_t pc)
      : mapping(m), function_name(fn_name), rel_pc(pc) {}
  Interner<Mapping>::Interned mapping;
  Interner<std::string>::Interned function_name;
  uint64_t rel_pc;

  bool operator<(const Frame& other) const {
    return std::tie(mapping, function_name, rel_pc) <
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

    Node(Interner<Frame>::Interned frame) : Node(std::move(frame), nullptr) {}
    Node(Interner<Frame>::Interned frame, Node* parent)
        : parent_(parent), location_(std::move(frame)) {}

    std::vector<Interner<Frame>::Interned> BuildCallstack() const;
    uintptr_t id() const { return reinterpret_cast<uintptr_t>(this); }

   private:
    Node* GetOrCreateChild(const Interner<Frame>::Interned& loc);

    uint64_t ref_count_ = 0;
    Node* const parent_;
    const Interner<Frame>::Interned location_;
    base::LookupSet<Node, const Interner<Frame>::Interned, &Node::location_>
        children_;
  };

  GlobalCallstackTrie() = default;
  GlobalCallstackTrie(const GlobalCallstackTrie&) = delete;
  GlobalCallstackTrie& operator=(const GlobalCallstackTrie&) = delete;

  Node* CreateCallsite(const std::vector<unwindstack::FrameData>& locs);
  static void DecrementNode(Node* node);
  static void IncrementNode(Node* node);

 private:
  Interner<Frame>::Interned InternCodeLocation(
      const unwindstack::FrameData& loc);
  Interner<Frame>::Interned MakeRootFrame();

  Interner<std::string> string_interner_;
  Interner<Mapping> mapping_interner_;
  Interner<Frame> frame_interner_;

  Node root_{MakeRootFrame()};
};

struct DumpState {
  void WriteMap(protos::pbzero::ProfilePacket* packet,
                const Interner<Mapping>::Interned map);
  void WriteFrame(protos::pbzero::ProfilePacket* packet,
                  const Interner<Frame>::Interned frame);
  void WriteString(protos::pbzero::ProfilePacket* packet,
                   const Interner<std::string>::Interned& str);

  std::set<InternID> dumped_strings;
  std::set<InternID> dumped_frames;
  std::set<InternID> dumped_mappings;
};

// Snapshot for memory allocations of a particular process. Shares callsites
// with other processes.
class HeapTracker {
 public:
  // Caller needs to ensure that callsites outlives the HeapTracker.
  explicit HeapTracker(GlobalCallstackTrie* callsites)
      : callsites_(callsites) {}

  void RecordMalloc(const std::vector<unwindstack::FrameData>& stack,
                    uint64_t address,
                    uint64_t size,
                    uint64_t sequence_number);
  void RecordFree(uint64_t address, uint64_t sequence_number);
  void Dump(protos::pbzero::ProfilePacket::ProcessHeapSamples* proto,
            std::set<GlobalCallstackTrie::Node*>* callstacks_to_dump);

  uint64_t GetSizeForTesting(const std::vector<unwindstack::FrameData>& stack);

 private:
  static constexpr uint64_t kNoopFree = 0;

  struct CallstackAllocations {
    CallstackAllocations(GlobalCallstackTrie::Node* n) : node(n) {}

    uint64_t allocated = 0;
    uint64_t freed = 0;
    uint64_t allocation_count = 0;
    uint64_t free_count = 0;

    GlobalCallstackTrie::Node* node;

    ~CallstackAllocations() {
      if (node)
        GlobalCallstackTrie::DecrementNode(node);
    }

    bool operator<(const CallstackAllocations& other) const {
      return node < other.node;
    }
  };

  struct Allocation {
    Allocation(uint64_t size, uint64_t seq, CallstackAllocations* csa)
        : total_size(size), sequence_number(seq), callstack_allocations(csa) {
      callstack_allocations->allocation_count++;
      callstack_allocations->allocated += total_size;
    }

    Allocation() = default;
    Allocation(const Allocation&) = delete;
    Allocation(Allocation&& other) noexcept {
      total_size = other.total_size;
      sequence_number = other.sequence_number;
      callstack_allocations = other.callstack_allocations;
      other.callstack_allocations = nullptr;
    }

    ~Allocation() {
      if (callstack_allocations) {
        callstack_allocations->free_count++;
        callstack_allocations->freed += total_size;
      }
    }

    uint64_t total_size;
    uint64_t sequence_number;
    CallstackAllocations* callstack_allocations;
  };

  // Sequencing logic works as following:
  // * mallocs are immediately commited to |allocations_|. They are rejected if
  //   the current malloc for the address has a higher sequence number.
  //
  //   If all operations with sequence numbers lower than the malloc have been
  //   commited to |allocations_|, sequence_number_ is advanced and all
  //   unblocked pending operations after the current id are commited to
  //   |allocations_|. Otherwise, a no-op record is added to the pending
  //   operations queue to maintain the contiguity of the sequence.

  // * for frees:
  //   if all operations with sequence numbers lower than the free have
  //     been commited to |allocations_| (i.e sequence_number_ ==
  //     sequence_number - 1) the free is commited to |allocations_| and
  //     sequence_number_ is advanced. All unblocked pending operations are
  //     commited to |allocations_|.
  //   otherwise: the free is added to the queue of pending operations.

  // Commits a free operation into |allocations_|.
  // This must be  called after all operations up to sequence_number have been
  // commited to |allocations_|.
  void CommitFree(uint64_t sequence_number, uint64_t address);

  // We cannot use an interner here, because after the last allocation goes
  // away, we still need to keep the CallstackAllocations around until the next
  // dump.
  std::map<GlobalCallstackTrie::Node*, CallstackAllocations>
      callstack_allocations_;

  std::vector<std::pair<decltype(callstack_allocations_)::iterator, uint64_t>>
      dead_callstack_allocations_;

  // Address -> (size, sequence_number, code location)
  std::map<uint64_t, Allocation> allocations_;

  // if allocation address != 0, there is pending free of the address.
  // if == 0, the pending operation is a no-op.
  // No-op operations come from allocs that have already been commited to
  // |allocations_|. It is important to keep track of them in the list of
  // pending to maintain the contiguity of the sequence.
  std::map<uint64_t /* seq_id */, uint64_t /* allocation address */>
      pending_frees_;

  // The sequence number all mallocs and frees have been handled up to.
  uint64_t sequence_number_ = 0;
  GlobalCallstackTrie* const callsites_;
};

struct BookkeepingData {
  // Ownership of callsites remains with caller and has to outlive this
  // object.
  explicit BookkeepingData(GlobalCallstackTrie* callsites)
      : heap_tracker(callsites) {}

  HeapTracker heap_tracker;

  // This is different to a shared_ptr to HeapTracker, because we want to keep
  // it around until the first dump after the last socket for the PID has
  // disconnected.
  uint64_t ref_count = 0;
};

// BookkeepingThread owns the BookkeepingData for all processes. The Run()
// method receives messages on the input_queue and does the bookkeeping.
class BookkeepingThread {
 public:
  friend class ProcessHandle;
  class ProcessHandle {
   public:
    friend class BookkeepingThread;
    friend void swap(ProcessHandle&, ProcessHandle&);

    ProcessHandle() = default;

    ~ProcessHandle();
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&&) noexcept;
    ProcessHandle& operator=(ProcessHandle&&) noexcept;

   private:
    ProcessHandle(BookkeepingThread* matcher, pid_t pid);

    BookkeepingThread* bookkeeping_thread_ = nullptr;
    pid_t pid_;
  };
  void Run(BoundedQueue<BookkeepingRecord>* input_queue);

  // Inform the bookkeeping thread that a socket for this pid connected.
  //
  // This can be called from arbitrary threads.
  ProcessHandle NotifyProcessConnected(pid_t pid);
  void HandleBookkeepingRecord(BookkeepingRecord* rec);

 private:
  // Inform the bookkeeping thread that a socket for this pid disconnected.
  // After the last client for a PID disconnects, the BookkeepingData is
  // retained until the next dump, upon which it gets garbage collected.
  //
  // This can be called from arbitrary threads.
  void NotifyProcessDisconnected(pid_t pid);

  GlobalCallstackTrie callsites_;

  std::map<pid_t, BookkeepingData> bookkeeping_data_;
  std::mutex bookkeeping_mutex_;
};

void swap(BookkeepingThread::ProcessHandle&, BookkeepingThread::ProcessHandle&);

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_BOOKKEEPING_H_
