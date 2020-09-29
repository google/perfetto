/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/dynamic/connected_flow_generator.h"

#include <memory>
#include <queue>
#include <set>

#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

ConnectedFlowGenerator::ConnectedFlowGenerator(Direction direction,
                                               TraceProcessorContext* context)
    : context_(context), direction_(direction) {}

ConnectedFlowGenerator::~ConnectedFlowGenerator() = default;

util::Status ConnectedFlowGenerator::ValidateConstraints(
    const QueryConstraints& qc) {
  const auto& cs = qc.constraints();

  auto flow_id_fn = [this](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(
                           context_->storage->flow_table().GetColumnCount()) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_flow_id_cs =
      std::find_if(cs.begin(), cs.end(), flow_id_fn) != cs.end();

  return has_flow_id_cs
             ? util::OkStatus()
             : util::ErrStatus("Failed to find required constraints");
}

std::vector<uint32_t> ConnectedFlowGenerator::GetConnectedFlowRows(
    SliceId start_id,
    Direction dir) {
  PERFETTO_DCHECK(dir != Direction::BOTH);
  std::vector<uint32_t> result_rows;

  // TODO: add hash function for SliceId and change this to unordered_set
  std::set<SliceId> visited_slice_ids;
  std::queue<SliceId> slice_id_queue({start_id});

  const auto& flow = context_->storage->flow_table();
  const TypedColumn<SliceId>& start_col =
      (dir == Direction::FOLLOWING ? flow.slice_out() : flow.slice_in());
  const TypedColumn<SliceId>& end_col =
      (dir == Direction::FOLLOWING ? flow.slice_in() : flow.slice_out());

  while (!slice_id_queue.empty()) {
    SliceId current_slice_id = slice_id_queue.front();
    slice_id_queue.pop();
    auto rows = flow.FilterToRowMap({start_col.eq(current_slice_id.value)});
    for (auto row_it = rows.IterateRows(); row_it; row_it.Next()) {
      SliceId next_slice_id = end_col[row_it.row()];
      if (visited_slice_ids.find(next_slice_id) != visited_slice_ids.end()) {
        continue;
      }

      visited_slice_ids.insert(next_slice_id);
      slice_id_queue.push(next_slice_id);
      result_rows.push_back(row_it.row());
    }
  }

  return result_rows;
}

std::unique_ptr<Table> ConnectedFlowGenerator::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>&) {
  const auto& flow = context_->storage->flow_table();
  const auto& slice = context_->storage->slice_table();

  auto it = std::find_if(cs.begin(), cs.end(), [&flow](const Constraint& c) {
    return c.col_idx == flow.GetColumnCount() && c.op == FilterOp::kEq;
  });

  PERFETTO_DCHECK(it != cs.end());

  SliceId start_id{static_cast<uint32_t>(it->value.AsLong())};

  if (!slice.id().IndexOf(start_id)) {
    PERFETTO_ELOG("Given slice id is invalid (ConnectedFlowGenerator)");
    return nullptr;
  }

  std::vector<uint32_t> result_rows;

  if (direction_ != Direction::PRECEDING) {
    // FOLLOWING or ALL_CONNECTED
    auto rows = GetConnectedFlowRows(start_id, Direction::FOLLOWING);
    result_rows.insert(result_rows.begin(), rows.begin(), rows.end());
  }
  if (direction_ != Direction::FOLLOWING) {
    // PRECEDING or ALL_CONNECTED
    auto rows = GetConnectedFlowRows(start_id, Direction::PRECEDING);
    result_rows.insert(result_rows.begin(), rows.begin(), rows.end());
  }

  // Aditional column for start_id
  std::unique_ptr<NullableVector<uint32_t>> start_ids(
      new NullableVector<uint32_t>());

  for (size_t i = 0; i < result_rows.size(); i++) {
    start_ids->Append(start_id.value);
  }

  return std::unique_ptr<Table>(
      new Table(flow.Apply(RowMap(std::move(result_rows)))
                    .ExtendWithColumn("start_id", std::move(start_ids),
                                      TypedColumn<uint32_t>::default_flags() |
                                          TypedColumn<uint32_t>::kHidden)));
}

Table::Schema ConnectedFlowGenerator::CreateSchema() {
  auto schema = tables::FlowTable::Schema();
  schema.columns.push_back(Table::Schema::Column{
      "start_id", SqlValue::Type::kLong, /* is_id = */ false,
      /* is_sorted = */ false, /* is_hidden = */ true});
  return schema;
}

std::string ConnectedFlowGenerator::TableName() {
  switch (direction_) {
    case Direction::BOTH:
      return "connected_flow";
    case Direction::FOLLOWING:
      return "following_flow";
    case Direction::PRECEDING:
      return "preceding_flow";
  }
  PERFETTO_FATAL("Unexpected ConnectedFlowType");
}

uint32_t ConnectedFlowGenerator::EstimateRowCount() {
  return 1;
}
}  // namespace trace_processor
}  // namespace perfetto
