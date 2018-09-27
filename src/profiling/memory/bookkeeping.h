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

#include "perfetto/base/lookup_set.h"
#include "src/profiling/memory/string_interner.h"

#include <map>
#include <string>
#include <vector>

namespace perfetto {

class HeapTracker;

struct CodeLocation {
  CodeLocation(std::string map_n, std::string function_n)
      : map_name(std::move(map_n)), function_name(std::move(function_n)) {}

  std::string map_name;
  std::string function_name;
};

// Internal data-structure for GlobalCallstackTrie to save memory if the same
// function is named multiple times.
struct InternedCodeLocation {
  StringInterner::InternedString map_name;
  StringInterner::InternedString function_name;

  bool operator<(const InternedCodeLocation& other) const {
    if (map_name.id() == other.map_name.id())
      return function_name.id() < other.function_name.id();
    return map_name.id() < other.map_name.id();
  }
};

// Graph of function callsites. This is shared between heap dumps for
// different processes. Each call site is represented by a
// GlobalCallstackTrie::Node that is owned by the parent (i.e. calling)
// callsite. It has a pointer to its parent, which means the function call-graph
// can be reconstructed from a GlobalCallstackTrie::Node by walking down the
// pointers to the parents.
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

    Node(InternedCodeLocation location) : Node(std::move(location), nullptr) {}
    Node(InternedCodeLocation location, Node* parent)
        : parent_(parent), location_(std::move(location)) {}

   private:
    Node* GetOrCreateChild(const InternedCodeLocation& loc);

    uint64_t cum_size_ = 0;
    Node* const parent_;
    const InternedCodeLocation location_;
    base::LookupSet<Node, const InternedCodeLocation, &Node::location_>
        children_;
  };

  GlobalCallstackTrie() = default;
  GlobalCallstackTrie(const GlobalCallstackTrie&) = delete;
  GlobalCallstackTrie& operator=(const GlobalCallstackTrie&) = delete;

  uint64_t GetCumSizeForTesting(const std::vector<CodeLocation>& stack);
  Node* IncrementCallsite(const std::vector<CodeLocation>& locs, uint64_t size);
  static void DecrementNode(Node* node, uint64_t size);

 private:
  InternedCodeLocation InternCodeLocation(const CodeLocation& loc) {
    return {interner_.Intern(loc.map_name),
            interner_.Intern(loc.function_name)};
  }

  StringInterner interner_;
  Node root_{{interner_.Intern(""), interner_.Intern("")}};
};

// Snapshot for memory allocations of a particular process. Shares callsites
// with other processes.
class HeapTracker {
 public:
  // Caller needs to ensure that callsites outlives the HeapTracker.
  explicit HeapTracker(GlobalCallstackTrie* callsites)
      : callsites_(callsites) {}

  void RecordMalloc(const std::vector<CodeLocation>& stack,
                    uint64_t address,
                    uint64_t size,
                    uint64_t sequence_number);
  void RecordFree(uint64_t address, uint64_t sequence_number);

 private:
  static constexpr uint64_t kNoopFree = 0;
  struct Allocation {
    Allocation(uint64_t size, uint64_t seq, GlobalCallstackTrie::Node* n)
        : alloc_size(size), sequence_number(seq), node(n) {}

    Allocation() = default;
    Allocation(const Allocation&) = delete;
    Allocation(Allocation&& other) noexcept {
      alloc_size = other.alloc_size;
      sequence_number = other.sequence_number;
      node = other.node;
      other.node = nullptr;
    }

    ~Allocation() {
      if (node)
        GlobalCallstackTrie::DecrementNode(node, alloc_size);
    }

    uint64_t alloc_size;
    uint64_t sequence_number;
    GlobalCallstackTrie::Node* node;
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

}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_BOOKKEEPING_H_
