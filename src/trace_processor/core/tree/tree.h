/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_CORE_TREE_TREE_H_
#define SRC_TRACE_PROCESSOR_CORE_TREE_TREE_H_

#include <cstdint>
#include <optional>

#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core::tree {

// Owns the memory for a tree structure and its associated column data.
//
// Design decisions:
// - NULL parent (root nodes) is represented as UINT32_MAX.
// - Trees are always compact: dense indices 0..n-1.
// - Column data is stored in a Dataframe for efficient storage and access.
struct Tree {
  static constexpr uint32_t kNoParent = UINT32_MAX;

  // Default constructor creates an empty tree.
  Tree() = default;

  // Move operations are supported.
  Tree(Tree&&) = default;
  Tree& operator=(Tree&&) = default;

  // Copy operations are deleted.
  Tree(const Tree&) = delete;
  Tree& operator=(const Tree&) = delete;

  // Returns the number of nodes in the tree.
  uint64_t size() const { return parents.size(); }

  // Parent index for each node (0..n-1). UINT32_MAX means root (no parent).
  Slab<uint32_t> parents;

  // Column data associated with each node, stored as a Dataframe.
  std::optional<dataframe::Dataframe> columns;
};

}  // namespace perfetto::trace_processor::core::tree

namespace perfetto::trace_processor {

// Namespace alias for ergonomics.
namespace tree = core::tree;

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_CORE_TREE_TREE_H_
