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

class MemoryBookkeeping {
 public:
  struct CodeLocation {
    CodeLocation(std::string map_n, std::string function_n)
        : map_name(std::move(map_n)), function_name(std::move(function_n)) {}

    std::string map_name;
    std::string function_name;
  };

  void RecordMalloc(const std::vector<CodeLocation>& stack,
                    uint64_t address,
                    uint64_t size);
  void RecordFree(uint64_t address);
  uint64_t GetCumSizeForTesting(const std::vector<CodeLocation>& stack);

 private:
  struct InternedCodeLocation {
    StringInterner::InternedString map_name;
    StringInterner::InternedString function_name;

    bool operator<(const InternedCodeLocation& other) const {
      if (map_name.id() == other.map_name.id())
        return function_name.id() < other.function_name.id();
      return map_name.id() < other.map_name.id();
    }
  };

  InternedCodeLocation InternCodeLocation(const CodeLocation& loc) {
    return {interner_.Intern(loc.map_name),
            interner_.Intern(loc.function_name)};
  }

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
    Node(InternedCodeLocation location) : Node(std::move(location), nullptr) {}
    Node(InternedCodeLocation location, Node* parent)
        : parent_(parent), location_(std::move(location)) {}

    Node* GetOrCreateChild(const InternedCodeLocation& loc);

    uint64_t cum_size_ = 0;
    Node* parent_;
    const InternedCodeLocation location_;
    base::LookupSet<Node, const InternedCodeLocation, &Node::location_>
        children_;
  };

  // Address -> (size, code location)
  std::map<uint64_t, std::pair<uint64_t, Node*>> allocations_;
  StringInterner interner_;
  Node root_{{interner_.Intern(""), interner_.Intern("")}};
};

}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_BOOKKEEPING_H_
