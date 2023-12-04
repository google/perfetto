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

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_flamegraph.h"

#include <unordered_set>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"

#include "src/trace_processor/importers/proto/heap_graph_tracker.h"
#include "src/trace_processor/importers/proto/heap_profile_tracker.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

namespace {

ExperimentalFlamegraph::ProfileType extractProfileType(
    std::string& profile_name) {
  if (profile_name == "graph") {
    return ExperimentalFlamegraph::ProfileType::kGraph;
  }
  if (profile_name == "native") {
    return ExperimentalFlamegraph::ProfileType::kHeapProfile;
  }
  if (profile_name == "perf") {
    return ExperimentalFlamegraph::ProfileType::kPerf;
  }
  PERFETTO_FATAL("Could not recognize profile type: %s.", profile_name.c_str());
}

bool IsValidTimestampOp(int op) {
  return sqlite_utils::IsOpEq(op) || sqlite_utils::IsOpGt(op) ||
         sqlite_utils::IsOpLe(op) || sqlite_utils::IsOpLt(op) ||
         sqlite_utils::IsOpGe(op);
}

bool IsValidFilterOp(FilterOp filterOp) {
  return filterOp == FilterOp::kEq || filterOp == FilterOp::kGt ||
         filterOp == FilterOp::kLe || filterOp == FilterOp::kLt ||
         filterOp == FilterOp::kGe;
}

// For filtering, this method uses the same constraints as
// ExperimentalFlamegraph::ValidateConstraints and should therefore
// be kept in sync.
ExperimentalFlamegraph::InputValues GetFlamegraphInputValues(
    const std::vector<Constraint>& cs) {
  using T = tables::ExperimentalFlamegraphNodesTable;

  auto ts_fn = [](const Constraint& c) {
    return c.col_idx == static_cast<uint32_t>(T::ColumnIndex::ts) &&
           IsValidFilterOp(c.op);
  };
  auto upid_fn = [](const Constraint& c) {
    return c.col_idx == static_cast<uint32_t>(T::ColumnIndex::upid) &&
           c.op == FilterOp::kEq;
  };
  auto upid_group_fn = [](const Constraint& c) {
    return c.col_idx == static_cast<uint32_t>(T::ColumnIndex::upid_group) &&
           c.op == FilterOp::kEq;
  };
  auto profile_type_fn = [](const Constraint& c) {
    return c.col_idx == static_cast<uint32_t>(T::ColumnIndex::profile_type) &&
           c.op == FilterOp::kEq;
  };
  auto focus_str_fn = [](const Constraint& c) {
    return c.col_idx == static_cast<uint32_t>(T::ColumnIndex::focus_str) &&
           c.op == FilterOp::kEq;
  };

  auto ts_it = std::find_if(cs.begin(), cs.end(), ts_fn);
  auto upid_it = std::find_if(cs.begin(), cs.end(), upid_fn);
  auto upid_group_it = std::find_if(cs.begin(), cs.end(), upid_group_fn);
  auto profile_type_it = std::find_if(cs.begin(), cs.end(), profile_type_fn);
  auto focus_str_it = std::find_if(cs.begin(), cs.end(), focus_str_fn);

  // We should always have valid iterators here because BestIndex should only
  // allow the constraint set to be chosen when we have an equality constraint
  // on upid and a constraint on ts.
  PERFETTO_CHECK(ts_it != cs.end());
  PERFETTO_CHECK(upid_it != cs.end() || upid_group_it != cs.end());
  PERFETTO_CHECK(profile_type_it != cs.end());

  std::string profile_name(profile_type_it->value.AsString());
  ExperimentalFlamegraph::ProfileType profile_type =
      extractProfileType(profile_name);
  int64_t ts = -1;
  std::vector<TimeConstraints> time_constraints = {};

  for (; ts_it != cs.end(); ts_it++) {
    if (ts_it->col_idx != static_cast<uint32_t>(T::ColumnIndex::ts)) {
      continue;
    }

    if (profile_type == ExperimentalFlamegraph::ProfileType::kPerf) {
      PERFETTO_CHECK(ts_it->op != FilterOp::kEq);
      time_constraints.push_back(
          TimeConstraints{ts_it->op, ts_it->value.AsLong()});
    } else {
      PERFETTO_CHECK(ts_it->op == FilterOp::kEq);
      ts = ts_it->value.AsLong();
    }
  }

  std::optional<UniquePid> upid;
  std::optional<std::string> upid_group;
  if (upid_it != cs.end()) {
    upid = static_cast<UniquePid>(upid_it->value.AsLong());
  } else {
    upid_group = upid_group_it->value.AsString();
  }

  std::string focus_str =
      focus_str_it != cs.end() ? focus_str_it->value.AsString() : "";
  return ExperimentalFlamegraph::InputValues{
      profile_type, ts,         std::move(time_constraints),
      upid,         upid_group, focus_str};
}

class Matcher {
 public:
  explicit Matcher(const std::string& str) : focus_str_(base::ToLower(str)) {}
  Matcher(const Matcher&) = delete;
  Matcher& operator=(const Matcher&) = delete;

  bool matches(const std::string& s) const {
    // TODO(149833691): change to regex.
    // We cannot use regex.h (does not exist in windows) or std regex (throws
    // exceptions).
    return base::Contains(base::ToLower(s), focus_str_);
  }

 private:
  const std::string focus_str_;
};

enum class FocusedState {
  kNotFocused,
  kFocusedPropagating,
  kFocusedNotPropagating,
};

using tables::ExperimentalFlamegraphNodesTable;
std::vector<FocusedState> ComputeFocusedState(
    const ExperimentalFlamegraphNodesTable& table,
    const Matcher& focus_matcher) {
  // Each row corresponds to a node in the flame chart tree with its parent
  // ptr. Root trees (no parents) will have a null parent ptr.
  std::vector<FocusedState> focused(table.row_count());

  for (uint32_t i = 0; i < table.row_count(); ++i) {
    auto parent_id = table.parent_id()[i];
    // Constraint: all descendants MUST come after their parents.
    PERFETTO_DCHECK(!parent_id.has_value() || *parent_id < table.id()[i]);

    if (focus_matcher.matches(table.name().GetString(i).ToStdString())) {
      // Mark as focused
      focused[i] = FocusedState::kFocusedPropagating;
      auto current = parent_id;
      // Mark all parent nodes as focused
      while (current.has_value()) {
        auto current_idx = *table.id().IndexOf(*current);
        if (focused[current_idx] != FocusedState::kNotFocused) {
          // We have already visited these nodes, skip
          break;
        }
        focused[current_idx] = FocusedState::kFocusedNotPropagating;
        current = table.parent_id()[current_idx];
      }
    } else if (parent_id.has_value() &&
               focused[*table.id().IndexOf(*parent_id)] ==
                   FocusedState::kFocusedPropagating) {
      // Focus cascades downwards.
      focused[i] = FocusedState::kFocusedPropagating;
    } else {
      focused[i] = FocusedState::kNotFocused;
    }
  }
  return focused;
}

struct CumulativeCounts {
  int64_t size;
  int64_t count;
  int64_t alloc_size;
  int64_t alloc_count;
};
std::unique_ptr<tables::ExperimentalFlamegraphNodesTable> FocusTable(
    TraceStorage* storage,
    std::unique_ptr<ExperimentalFlamegraphNodesTable> in,
    const std::string& focus_str) {
  if (in->row_count() == 0 || focus_str.empty()) {
    return in;
  }
  std::vector<FocusedState> focused_state =
      ComputeFocusedState(*in, Matcher(focus_str));
  std::unique_ptr<ExperimentalFlamegraphNodesTable> tbl(
      new tables::ExperimentalFlamegraphNodesTable(
          storage->mutable_string_pool()));

  // Recompute cumulative counts
  std::vector<CumulativeCounts> node_to_cumulatives(in->row_count());
  for (int64_t idx = in->row_count() - 1; idx >= 0; --idx) {
    auto i = static_cast<uint32_t>(idx);
    if (focused_state[i] == FocusedState::kNotFocused) {
      continue;
    }
    auto& cumulatives = node_to_cumulatives[i];
    cumulatives.size += in->size()[i];
    cumulatives.count += in->count()[i];
    cumulatives.alloc_size += in->alloc_size()[i];
    cumulatives.alloc_count += in->alloc_count()[i];

    auto parent_id = in->parent_id()[i];
    if (parent_id.has_value()) {
      auto& parent_cumulatives =
          node_to_cumulatives[*in->id().IndexOf(*parent_id)];
      parent_cumulatives.size += cumulatives.size;
      parent_cumulatives.count += cumulatives.count;
      parent_cumulatives.alloc_size += cumulatives.alloc_size;
      parent_cumulatives.alloc_count += cumulatives.alloc_count;
    }
  }

  // Mapping between the old rows ('node') to the new identifiers.
  std::vector<ExperimentalFlamegraphNodesTable::Id> node_to_id(in->row_count());
  for (uint32_t i = 0; i < in->row_count(); ++i) {
    if (focused_state[i] == FocusedState::kNotFocused) {
      continue;
    }

    tables::ExperimentalFlamegraphNodesTable::Row alloc_row{};
    // We must reparent the rows as every insertion will get its own
    // identifier.
    auto original_parent_id = in->parent_id()[i];
    if (original_parent_id.has_value()) {
      auto original_idx = *in->id().IndexOf(*original_parent_id);
      alloc_row.parent_id = node_to_id[original_idx];
    }

    alloc_row.ts = in->ts()[i];
    alloc_row.upid = in->upid()[i];
    alloc_row.profile_type = in->profile_type()[i];
    alloc_row.depth = in->depth()[i];
    alloc_row.name = in->name()[i];
    alloc_row.map_name = in->map_name()[i];
    alloc_row.count = in->count()[i];
    alloc_row.size = in->size()[i];
    alloc_row.alloc_count = in->alloc_count()[i];
    alloc_row.alloc_size = in->alloc_size()[i];

    const auto& cumulative = node_to_cumulatives[i];
    alloc_row.cumulative_count = cumulative.count;
    alloc_row.cumulative_size = cumulative.size;
    alloc_row.cumulative_alloc_count = cumulative.alloc_count;
    alloc_row.cumulative_alloc_size = cumulative.alloc_size;
    node_to_id[i] = tbl->Insert(alloc_row).id;
  }
  return tbl;
}
}  // namespace

ExperimentalFlamegraph::ExperimentalFlamegraph(TraceProcessorContext* context)
    : context_(context) {}

ExperimentalFlamegraph::~ExperimentalFlamegraph() = default;

// For filtering, this method uses the same constraints as
// ExperimentalFlamegraph::GetFlamegraphInputValues and should
// therefore be kept in sync.
base::Status ExperimentalFlamegraph::ValidateConstraints(
    const QueryConstraints& qc) {
  using T = tables::ExperimentalFlamegraphNodesTable;

  const auto& cs = qc.constraints();

  auto ts_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::ts) &&
           IsValidTimestampOp(c.op);
  };
  bool has_ts_cs = std::find_if(cs.begin(), cs.end(), ts_fn) != cs.end();

  auto upid_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::upid) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_upid_cs = std::find_if(cs.begin(), cs.end(), upid_fn) != cs.end();

  auto upid_group_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::upid_group) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_upid_group_cs =
      std::find_if(cs.begin(), cs.end(), upid_group_fn) != cs.end();

  auto profile_type_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::profile_type) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_profile_type_cs =
      std::find_if(cs.begin(), cs.end(), profile_type_fn) != cs.end();

  return has_ts_cs && (has_upid_cs || has_upid_group_cs) && has_profile_type_cs
             ? base::OkStatus()
             : base::ErrStatus("Failed to find required constraints");
}

base::Status ExperimentalFlamegraph::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>&,
    const BitVector&,
    std::unique_ptr<Table>& table_return) {
  // Get the input column values and compute the flamegraph using them.
  auto values = GetFlamegraphInputValues(cs);

  std::unique_ptr<tables::ExperimentalFlamegraphNodesTable> table;
  if (values.profile_type == ProfileType::kGraph) {
    auto* tracker = HeapGraphTracker::GetOrCreate(context_);
    table = tracker->BuildFlamegraph(values.ts, *values.upid);
  } else if (values.profile_type == ProfileType::kHeapProfile) {
    table = BuildHeapProfileFlamegraph(context_->storage.get(), *values.upid,
                                       values.ts);
  } else if (values.profile_type == ProfileType::kPerf) {
    table = BuildNativeCallStackSamplingFlamegraph(
        context_->storage.get(), values.upid, values.upid_group,
        values.time_constraints);
  }
  if (!table) {
    return base::ErrStatus("Failed to build flamegraph");
  }
  if (!values.focus_str.empty()) {
    table =
        FocusTable(context_->storage.get(), std::move(table), values.focus_str);
    // The pseudocolumns must be populated because as far as SQLite is
    // concerned these are equality constraints.
    auto focus_id =
        context_->storage->InternString(base::StringView(values.focus_str));
    for (uint32_t i = 0; i < table->row_count(); ++i) {
      table->mutable_focus_str()->Set(i, focus_id);
    }
  }
  table_return = std::move(table);
  return base::OkStatus();
}

Table::Schema ExperimentalFlamegraph::CreateSchema() {
  return tables::ExperimentalFlamegraphNodesTable::ComputeStaticSchema();
}

std::string ExperimentalFlamegraph::TableName() {
  return "experimental_flamegraph";
}

uint32_t ExperimentalFlamegraph::EstimateRowCount() {
  // TODO(lalitm): return a better estimate here when possible.
  return 1024;
}

}  // namespace trace_processor
}  // namespace perfetto
