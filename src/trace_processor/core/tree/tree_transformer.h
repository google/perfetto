/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_CORE_TREE_TREE_TRANSFORMER_H_
#define SRC_TRACE_PROCESSOR_CORE_TREE_TREE_TRANSFORMER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"

namespace perfetto::trace_processor::core::interpreter {
class BytecodeBuilder;
}  // namespace perfetto::trace_processor::core::interpreter

namespace perfetto::trace_processor::core::tree {

// Transforms a tree-structured dataframe via operations (currently filtering)
// producing another dataframe.
//
// The tree structure is represented by the first two columns of the dataframe:
// - Column 0: node ID
// - Column 1: parent ID
//
// Bytecodes are emitted incrementally as methods are called. ToDataframe()
// finalizes the bytecode, executes it, and returns the result.
//
// Usage:
//   TreeTransformer t(df, pool);
//   RETURN_IF_ERROR(t.FilterTree(specs, values));
//   ASSIGN_OR_RETURN(auto result, std::move(t).ToDataframe());
class TreeTransformer {
 public:
  TreeTransformer(dataframe::Dataframe df, StringPool* pool);
  ~TreeTransformer();

  TreeTransformer(TreeTransformer&&) noexcept;
  TreeTransformer& operator=(TreeTransformer&&) noexcept;

  // Applies a filter to the tree. Nodes matching the filter are kept;
  // filtered-out nodes have their children reparented to the closest
  // surviving ancestor. Can be called multiple times; bytecodes are
  // emitted immediately.
  base::Status FilterTree(std::vector<dataframe::FilterSpec> specs,
                          std::vector<SqlValue> values);

  const dataframe::Dataframe& df() const { return df_; }

  // Finalizes bytecode, executes it, and returns the resulting dataframe.
  base::StatusOr<dataframe::Dataframe> ToDataframe() &&;

 private:
  struct RegInit {
    enum Kind : uint8_t { kStorage, kNullBv };
    Kind kind;
    uint32_t reg;
    uint32_t col;
  };

  dataframe::Dataframe df_;
  StringPool* pool_;

  // Bytecode builder — accumulates bytecodes as methods are called.
  // Heap-allocated so the header doesn't need to include bytecode_builder.h.
  std::unique_ptr<interpreter::BytecodeBuilder> builder_;

  // Register indices (allocated in constructor, shared across calls).
  uint32_t span_reg_index_ = 0;
  uint32_t tree_state_reg_index_ = 0;

  // Register initialization info collected during FilterTree calls.
  std::vector<RegInit> reg_inits_;

  // Filter values accumulated across FilterTree calls.
  std::vector<SqlValue> filter_values_;
  uint32_t filter_value_count_ = 0;

  // Whether any filter bytecodes have been emitted.
  bool has_filters_ = false;
};

}  // namespace perfetto::trace_processor::core::tree

namespace perfetto::trace_processor {
namespace tree = core::tree;
}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_CORE_TREE_TREE_TRANSFORMER_H_
