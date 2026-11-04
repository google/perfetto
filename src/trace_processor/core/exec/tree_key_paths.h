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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_KEY_PATHS_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_KEY_PATHS_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// The columns holding the tree structure, and the columns the paths are made
// of. Node and parent columns must be flat, non-null Uint32 columns.
struct TreeKeyPathsSpec {
  uint32_t node_column = 0;
  uint32_t parent_column = 1;
  std::vector<uint32_t> key_columns;
};

// Numbers each node by its root-to-node path of the key columns: two nodes get
// the same number when their parents do and they agree on the key columns.
// Numbers are dense and given out in the order paths are first seen.
//
// Requires rows parent first. Input columns are preserved and two Int64 columns
// are appended: the node's number, and its parent's, null for a root.
class TreeKeyPaths final : public Transform {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns* out) const override {
    out->reads = spec_.key_columns;
    out->reads.push_back(spec_.node_column);
    out->reads.push_back(spec_.parent_column);
    out->added = 2;
  }

  explicit TreeKeyPaths(TreeKeyPathsSpec);
  ~TreeKeyPaths() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  bool Process(RowBatch&, OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  friend class PipelineFormatter;

  TreeKeyPathsSpec spec_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_KEY_PATHS_H_
