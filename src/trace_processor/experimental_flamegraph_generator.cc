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

#include "src/trace_processor/experimental_flamegraph_generator.h"

#include "src/trace_processor/heap_profile_tracker.h"
#include "src/trace_processor/importers/proto/heap_graph_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

namespace {

ExperimentalFlamegraphGenerator::InputValues GetInputValues(
    const std::vector<Constraint>& cs) {
  using T = tables::ExperimentalFlamegraphNodesTable;

  auto ts_fn = [](const Constraint& c) {
    return c.col_idx == static_cast<uint32_t>(T::ColumnIndex::ts) &&
           c.op == FilterOp::kEq;
  };
  auto upid_fn = [](const Constraint& c) {
    return c.col_idx == static_cast<uint32_t>(T::ColumnIndex::upid) &&
           c.op == FilterOp::kEq;
  };
  auto profile_type_fn = [](const Constraint& c) {
    return c.col_idx == static_cast<uint32_t>(T::ColumnIndex::profile_type) &&
           c.op == FilterOp::kEq;
  };

  auto ts_it = std::find_if(cs.begin(), cs.end(), ts_fn);
  auto upid_it = std::find_if(cs.begin(), cs.end(), upid_fn);
  auto profile_type_it = std::find_if(cs.begin(), cs.end(), profile_type_fn);

  // We should always have valid iterators here because BestIndex should only
  // allow the constraint set to be chosen when we have an equality constraint
  // on both ts and upid.
  PERFETTO_CHECK(ts_it != cs.end());
  PERFETTO_CHECK(upid_it != cs.end());
  PERFETTO_CHECK(profile_type_it != cs.end());

  int64_t ts = ts_it->value.AsLong();
  UniquePid upid = static_cast<UniquePid>(upid_it->value.AsLong());
  std::string profile_type = profile_type_it->value.AsString();

  return ExperimentalFlamegraphGenerator::InputValues{ts, upid, profile_type};
}

}  // namespace

ExperimentalFlamegraphGenerator::ExperimentalFlamegraphGenerator(
    TraceProcessorContext* context)
    : context_(context) {}

ExperimentalFlamegraphGenerator::~ExperimentalFlamegraphGenerator() = default;

util::Status ExperimentalFlamegraphGenerator::ValidateConstraints(
    const QueryConstraints& qc) {
  using T = tables::ExperimentalFlamegraphNodesTable;

  const auto& cs = qc.constraints();

  auto ts_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::ts) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_ts_cs = std::find_if(cs.begin(), cs.end(), ts_fn) != cs.end();

  auto upid_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::upid) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_upid_cs = std::find_if(cs.begin(), cs.end(), upid_fn) != cs.end();

  auto profile_type_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::profile_type) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_profile_type_cs =
      std::find_if(cs.begin(), cs.end(), profile_type_fn) != cs.end();

  return has_ts_cs && has_upid_cs && has_profile_type_cs
             ? util::OkStatus()
             : util::ErrStatus("Failed to find required constraints");
}

Table* ExperimentalFlamegraphGenerator::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>&) {
  // Get the input column values and compute the flamegraph using them.
  auto values = GetInputValues(cs);

  if (values.profile_type == "graph") {
    auto* tracker = HeapGraphTracker::GetOrCreate(context_);
    table_ = tracker->BuildFlamegraph(values.ts, values.upid);
  }
  if (values.profile_type == "native") {
    table_ =
        BuildNativeFlamegraph(context_->storage.get(), values.upid, values.ts);
  }
  return table_.get();
}

Table::Schema ExperimentalFlamegraphGenerator::CreateSchema() {
  return tables::ExperimentalFlamegraphNodesTable::Schema();
}

std::string ExperimentalFlamegraphGenerator::TableName() {
  return "experimental_flamegraph";
}

uint32_t ExperimentalFlamegraphGenerator::EstimateRowCount() {
  // TODO(lalitm): return a better estimate here when possible.
  return 1024;
}

}  // namespace trace_processor
}  // namespace perfetto
