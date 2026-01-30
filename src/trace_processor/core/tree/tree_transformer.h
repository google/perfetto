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

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"

namespace perfetto::trace_processor::core::tree {

// Transforms a tree-structured dataframe via a bunch of operations producing
// another dataframe.
class TreeTransformer {
 public:
  explicit TreeTransformer(dataframe::Dataframe df, StringPool* pool);

  // Transforms the tree and returns the resulting dataframe.
  base::StatusOr<dataframe::Dataframe> ToDataframe() &&;

 private:
  dataframe::Dataframe df_;
  StringPool* pool_;
};

}  // namespace perfetto::trace_processor::core::tree

namespace perfetto::trace_processor {

// Namespace alias for ergonomics.
namespace tree = core::tree;

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_CORE_TREE_TREE_TRANSFORMER_H_
