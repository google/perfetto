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

#ifndef SRC_TRACE_PROCESSOR_CORE_TREE_TREE_BUILDER_H_
#define SRC_TRACE_PROCESSOR_CORE_TREE_TREE_BUILDER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/interpreter/bytecode_core.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor {
class StringPool;
}

namespace perfetto::trace_processor::core::tree {

// Builder for accumulating lazy tree transformations.
// Transformations are stored as bytecode and executed when Build() is called.
//
// Usage:
//   TreeTransformationBuilder builder(std::move(tree));
//   builder.Filter("column_name", Eq{}, value);
//   auto result = std::move(builder).Build();
class TreeTransformationBuilder {
 public:
  // Creates a builder from a tree.
  explicit TreeTransformationBuilder(std::unique_ptr<Tree> tree);

  // Move operations are supported.
  TreeTransformationBuilder(TreeTransformationBuilder&&) = default;
  TreeTransformationBuilder& operator=(TreeTransformationBuilder&&) = default;

  // Copy operations are deleted.
  TreeTransformationBuilder(const TreeTransformationBuilder&) = delete;
  TreeTransformationBuilder& operator=(const TreeTransformationBuilder&) =
      delete;

  using FilterValue = std::variant<int64_t, double, std::string>;
  using FilterOp = TypeSet<Eq, Ne, Lt, Le, Gt, Ge>;

  // Filter nodes based on column comparison.
  // Nodes not matching the filter are removed; their children are reparented
  // to the nearest surviving ancestor.
  // Returns false if column not found.
  bool Filter(const std::string& column_name,
              FilterOp op,
              const FilterValue& value);

  // Execute all accumulated transformations and return the resulting tree.
  // Consumes the builder.
  std::unique_ptr<Tree> Build() &&;

  // Execute all accumulated transformations and return as a dataframe.
  // Calls Build() internally, then converts the tree to a dataframe with
  // _tree_id and _tree_parent_id columns plus all original tree columns.
  // Consumes the builder.
  base::StatusOr<dataframe::Dataframe> BuildDataframe(StringPool* pool) &&;

  // Returns the accumulated bytecode for testing purposes.
  const interpreter::BytecodeVector& GetBytecodeForTesting() const {
    return bytecode_;
  }

 private:
  // Adds a bytecode instruction of type T and returns a reference to it.
  template <typename T>
  T& AddOpcode() {
    bytecode_.emplace_back(T{});
    bytecode_.back().option = interpreter::Index<T>();
    return static_cast<T&>(bytecode_.back());
  }

  // Ensures CSR structure is built (adds bytecode if needed).
  void EnsureCsr();

  // Converts a tree to a dataframe with _tree_id and _tree_parent_id columns.
  static base::StatusOr<dataframe::Dataframe> TreeToDataframe(
      std::unique_ptr<Tree> tree,
      StringPool* pool);

  // The base tree (owned).
  std::unique_ptr<Tree> base_;

  // Accumulated bytecode from lazy operations.
  interpreter::BytecodeVector bytecode_;

  // Tracks which registers hold what data.
  struct RegisterState {
    // Register holding ChildToParent structure.
    uint32_t child_to_parent_reg = 0;
    // Register holding ParentToChild (CSR) structure.
    uint32_t parent_to_child_reg = 0;
    // Whether CSR has been built.
    bool csr_valid = false;
    // Next available register index.
    uint32_t next_reg = 0;
  };
  RegisterState reg_state_;
};

}  // namespace perfetto::trace_processor::core::tree

#endif  // SRC_TRACE_PROCESSOR_CORE_TREE_TREE_BUILDER_H_
