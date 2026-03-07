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

#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"

namespace perfetto::trace_processor::core::tree {

// Transforms a tree-structured dataframe via filtering operations producing
// another dataframe.
//
// The tree structure is represented by the first two columns of the dataframe:
// - Column 0: node ID
// - Column 1: parent ID
//
// Filtering reuses the standard QueryPlanBuilder::FilterOnly() to emit filter
// bytecodes that operate on the original dataframe storage. A single
// FilterTreeState bytecode then reparents and compacts the tree structure.
//
// Usage:
//   TreeTransformer t(df, pool);
//   RETURN_IF_ERROR(t.FilterTree(specs, values));
//   ASSIGN_OR_RETURN(auto result, std::move(t).ToDataframe());
class TreeTransformer {
 public:
  explicit TreeTransformer(dataframe::Dataframe df, StringPool* pool);

  // Applies a filter to the tree. Nodes matching the filter are kept;
  // filtered-out nodes have their children reparented to the closest
  // surviving ancestor. Can be called multiple times.
  base::Status FilterTree(std::vector<dataframe::FilterSpec> specs,
                          std::vector<SqlValue> values);

  const dataframe::Dataframe& df() const { return df_; }

  // Builds and executes bytecode, returns the resulting dataframe.
  base::StatusOr<dataframe::Dataframe> ToDataframe() &&;

 private:
  dataframe::Dataframe df_;
  StringPool* pool_;

  // Accumulated filter specs and values.
  std::vector<dataframe::FilterSpec> filter_specs_;
  std::vector<SqlValue> filter_values_;
};

}  // namespace perfetto::trace_processor::core::tree

namespace perfetto::trace_processor {
namespace tree = core::tree;
}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_CORE_TREE_TREE_TRANSFORMER_H_
