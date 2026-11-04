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

#include "src/trace_processor/core/exec/tree_key_paths.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_held.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

class KeyPathsState : public OperatorState {
 public:
  explicit KeyPathsState(Context& c) : context(&c) {}
  ~KeyPathsState() override;

  Context* context;

  Held<base::Status> status{this};

  // Numbers the distinct values of the key columns.
  Held<GroupTable> values{this};
  // Numbers paths, by their parent's path and their number in `values`.
  Held<base::FlatHashMap<uint64_t, uint32_t>> paths{this};
  // The path of every node seen.
  Held<FlexVector<uint32_t>> node_paths{this};

  // Each row's number in `values`.
  CallScratch<FlexVector<uint32_t>> row_values;
};

KeyPathsState::~KeyPathsState() = default;

}  // namespace

TreeKeyPaths::TreeKeyPaths(TreeKeyPathsSpec spec) : spec_(std::move(spec)) {}
TreeKeyPaths::~TreeKeyPaths() = default;

std::unique_ptr<OperatorState> TreeKeyPaths::MakeState(Context& context) const {
  return std::make_unique<KeyPathsState>(context);
}

base::Status TreeKeyPaths::status(const OperatorState& state) const {
  return state.Cast<const KeyPathsState>().status;
}

bool TreeKeyPaths::Process(RowBatch& batch, OperatorState& state) const {
  auto& s = state.Cast<KeyPathsState>();
  const Selection& selection = batch.selection();
  uint32_t count = batch.size();

  s.row_values.resize(count);
  if (std::optional<uint32_t> bad = s.values.FindOrCreate(
          batch, spec_.key_columns, s.row_values.data())) {
    s.status = base::ErrStatus(
        "TREE KEY PATHS: key %u must be a column holding one type", *bad + 1);
    return false;
  }

  ColumnBuffer path_buffer = s.context->TakeBuffer(sizeof(int64_t));
  int64_t* path_values = path_buffer.chunk().Values<int64_t>();
  ColumnBuffer parent_buffer = s.context->TakeBuffer(sizeof(int64_t));
  ColumnChunk& parent_chunk = parent_buffer.chunk();
  int64_t* parent_values = parent_chunk.Values<int64_t>();
  parent_chunk.SizeValidity();

  FlatColumnReader<uint32_t> nodes(batch, spec_.node_column);
  FlatColumnReader<uint32_t> parents(batch, spec_.parent_column);
  // Rows are parent first, so each row's parent already has its path.
  for (uint32_t row = 0; row < count; ++row) {
    uint32_t node = 0;
    uint32_t parent = kNoNode;
    nodes.Read(row, &node);
    parents.Read(row, &parent);
    uint32_t batch_row = selection[row];

    uint64_t path_key = s.row_values[row];
    bool is_root = parent == kNoNode;
    parent_chunk.validity.change(batch_row, !is_root);
    if (!is_root) {
      PERFETTO_DCHECK(parent < s.node_paths.size());
      uint32_t parent_path = s.node_paths[parent];
      parent_values[batch_row] = parent_path;
      path_key |= (uint64_t{parent_path} + 1) << 32;
    }
    auto next = static_cast<uint32_t>(s.paths.size());
    uint32_t path = *s.paths.Insert(path_key, next).first;
    path_values[batch_row] = path;
    if (node >= s.node_paths.size()) {
      s.node_paths.push_back_multiple(0, node + 1 - s.node_paths.size());
    }
    s.node_paths[node] = path;
  }

  batch.AddColumn(ColumnView::Reference(StorageType{Int64{}}, path_values),
                  std::move(path_buffer));
  batch.AddColumn(ColumnView::Reference(StorageType{Int64{}}, parent_values,
                                        &parent_chunk.validity),
                  std::move(parent_buffer));
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
