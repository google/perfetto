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

#include "src/trace_processor/dynamic/ancestor_generator.h"
#include "src/trace_processor/dynamic/descendant_generator.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

ConnectedFlowTable::~ConnectedFlowTable() = default;

}  // namespace tables

ConnectedFlowGenerator::ConnectedFlowGenerator(Mode mode,
                                               const TraceStorage* storage)
    : mode_(mode), storage_(storage) {}

ConnectedFlowGenerator::~ConnectedFlowGenerator() = default;

base::Status ConnectedFlowGenerator::ValidateConstraints(
    const QueryConstraints& qc) {
  const auto& cs = qc.constraints();

  auto flow_id_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(
                           tables::ConnectedFlowTable::ColumnIndex::start_id) &&
           sqlite_utils::IsOpEq(c.op);
  };
  bool has_flow_id_cs =
      std::find_if(cs.begin(), cs.end(), flow_id_fn) != cs.end();

  return has_flow_id_cs
             ? base::OkStatus()
             : base::ErrStatus("Failed to find required constraints");
}

namespace {

enum FlowVisitMode : uint8_t {
  VISIT_INCOMING = 1 << 0,
  VISIT_OUTGOING = 1 << 1,
  VISIT_INCOMING_AND_OUTGOING = VISIT_INCOMING | VISIT_OUTGOING,
};

enum RelativesVisitMode : uint8_t {
  VISIT_NO_RELATIVES = 0,
  VISIT_ANCESTORS = 1 << 0,
  VISIT_DESCENDANTS = 1 << 1,
  VISIT_ALL_RELATIVES = VISIT_ANCESTORS | VISIT_DESCENDANTS,
};

// Searches through the slice table recursively to find connected flows.
// Usage:
//  BFS bfs = BFS(context);
//  bfs
//    // Add list of slices to start with.
//    .Start(start_id).Start(start_id2)
//    // Additionally include relatives of |another_id| in search space.
//    .GoToRelatives(another_id, VISIT_ANCESTORS)
//    // Visit all connected slices to the above slices.
//    .VisitAll(VISIT_INCOMING, VISIT_NO_RELATIVES);
//
//  bfs.TakeResultingFlows();
class BFS {
 public:
  explicit BFS(const TraceStorage* storage) : storage_(storage) {}

  std::vector<tables::FlowTable::RowNumber> TakeResultingFlows() && {
    return std::move(flow_rows_);
  }

  // Includes a starting slice ID to search.
  BFS& Start(SliceId start_id) {
    slices_to_visit_.push({start_id, VisitType::START});
    known_slices_.insert(start_id);
    return *this;
  }

  // Visits all slices that can be reached from the given starting slices.
  void VisitAll(FlowVisitMode visit_flow, RelativesVisitMode visit_relatives) {
    while (!slices_to_visit_.empty()) {
      SliceId slice_id = slices_to_visit_.front().first;
      VisitType visit_type = slices_to_visit_.front().second;
      slices_to_visit_.pop();

      // If the given slice is being visited due to being ancestor or descendant
      // of a previous one, do not compute ancestors or descendants again as the
      // result is going to be the same.
      if (visit_type != VisitType::VIA_RELATIVE) {
        GoToRelatives(slice_id, visit_relatives);
      }

      // If the slice was visited by a flow, do not try to go back.
      if ((visit_flow & VISIT_INCOMING) &&
          visit_type != VisitType::VIA_OUTGOING_FLOW) {
        GoByFlow(slice_id, FlowDirection::INCOMING);
      }
      if ((visit_flow & VISIT_OUTGOING) &&
          visit_type != VisitType::VIA_INCOMING_FLOW) {
        GoByFlow(slice_id, FlowDirection::OUTGOING);
      }
    }
  }

  // Includes the relatives of |slice_id| to the list of slices to visit.
  BFS& GoToRelatives(SliceId slice_id, RelativesVisitMode visit_relatives) {
    const auto& slice_table = storage_->slice_table();
    if (visit_relatives & VISIT_ANCESTORS) {
      auto opt_ancestors =
          AncestorGenerator::GetAncestorSlices(slice_table, slice_id);
      if (opt_ancestors)
        GoToRelativesImpl(*opt_ancestors);
    }
    if (visit_relatives & VISIT_DESCENDANTS) {
      auto opt_descendants =
          DescendantGenerator::GetDescendantSlices(slice_table, slice_id);
      if (opt_descendants)
        GoToRelativesImpl(*opt_descendants);
    }
    return *this;
  }

 private:
  enum class FlowDirection {
    INCOMING,
    OUTGOING,
  };

  enum class VisitType {
    START,
    VIA_INCOMING_FLOW,
    VIA_OUTGOING_FLOW,
    VIA_RELATIVE,
  };

  void GoByFlow(SliceId slice_id, FlowDirection flow_direction) {
    PERFETTO_DCHECK(known_slices_.count(slice_id) != 0);

    const auto& flow = storage_->flow_table();

    const TypedColumn<SliceId>& start_col =
        flow_direction == FlowDirection::OUTGOING ? flow.slice_out()
                                                  : flow.slice_in();
    auto it = flow.FilterToIterator({start_col.eq(slice_id.value)});
    for (; it; ++it) {
      flow_rows_.push_back(it.row_number());

      SliceId next_slice_id = flow_direction == FlowDirection::OUTGOING
                                  ? it.slice_in()
                                  : it.slice_out();
      if (known_slices_.count(next_slice_id))
        continue;

      known_slices_.insert(next_slice_id);
      slices_to_visit_.push(
          {next_slice_id, flow_direction == FlowDirection::INCOMING
                              ? VisitType::VIA_INCOMING_FLOW
                              : VisitType::VIA_OUTGOING_FLOW});
    }
  }

  void GoToRelativesImpl(
      const std::vector<tables::SliceTable::RowNumber>& rows) {
    const auto& slice = storage_->slice_table();
    for (tables::SliceTable::RowNumber row : rows) {
      auto relative_slice_id = row.ToRowReference(slice).id();
      if (known_slices_.count(relative_slice_id))
        continue;
      known_slices_.insert(relative_slice_id);
      slices_to_visit_.push({relative_slice_id, VisitType::VIA_RELATIVE});
    }
  }

  std::queue<std::pair<SliceId, VisitType>> slices_to_visit_;
  std::set<SliceId> known_slices_;
  std::vector<tables::FlowTable::RowNumber> flow_rows_;

  const TraceStorage* storage_;
};

}  // namespace

base::Status ConnectedFlowGenerator::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>&,
    const BitVector&,
    std::unique_ptr<Table>& table_return) {
  const auto& flow = storage_->flow_table();
  const auto& slice = storage_->slice_table();

  auto it = std::find_if(cs.begin(), cs.end(), [](const Constraint& c) {
    return c.col_idx == tables::ConnectedFlowTable::ColumnIndex::start_id &&
           c.op == FilterOp::kEq;
  });
  if (it == cs.end()) {
    return base::ErrStatus("no start id specified.");
  }
  if (it->value.type == SqlValue::Type::kNull) {
    // Nothing matches a null id so return an empty table.
    table_return =
        tables::ConnectedFlowTable::SelectAndExtendParent(flow, {}, {});
    return base::OkStatus();
  }
  if (it->value.type != SqlValue::Type::kLong) {
    return base::ErrStatus("start id should be an integer.");
  }

  SliceId start_id{static_cast<uint32_t>(it->value.AsLong())};

  if (!slice.id().IndexOf(start_id)) {
    return base::ErrStatus("invalid slice id %" PRIu32 "",
                           static_cast<uint32_t>(start_id.value));
  }

  BFS bfs(storage_);

  switch (mode_) {
    case Mode::kDirectlyConnectedFlow:
      bfs.Start(start_id).VisitAll(VISIT_INCOMING_AND_OUTGOING,
                                   VISIT_NO_RELATIVES);
      break;
    case Mode::kFollowingFlow:
      bfs.Start(start_id).VisitAll(VISIT_OUTGOING, VISIT_DESCENDANTS);
      break;
    case Mode::kPrecedingFlow:
      bfs.Start(start_id).VisitAll(VISIT_INCOMING, VISIT_ANCESTORS);
      break;
  }

  std::vector<tables::FlowTable::RowNumber> result_rows =
      std::move(bfs).TakeResultingFlows();

  // Aditional column for start_id
  ColumnStorage<uint32_t> start_ids;
  for (size_t i = 0; i < result_rows.size(); i++) {
    start_ids.Append(start_id.value);
  }
  table_return = tables::ConnectedFlowTable::SelectAndExtendParent(
      flow, result_rows, std::move(start_ids));
  return base::OkStatus();
}

Table::Schema ConnectedFlowGenerator::CreateSchema() {
  return tables::ConnectedFlowTable::ComputeStaticSchema();
}

std::string ConnectedFlowGenerator::TableName() {
  switch (mode_) {
    case Mode::kDirectlyConnectedFlow:
      return "directly_connected_flow";
    case Mode::kFollowingFlow:
      return "following_flow";
    case Mode::kPrecedingFlow:
      return "preceding_flow";
  }
  PERFETTO_FATAL("Unexpected ConnectedFlowType");
}

uint32_t ConnectedFlowGenerator::EstimateRowCount() {
  return 1;
}
}  // namespace trace_processor
}  // namespace perfetto
