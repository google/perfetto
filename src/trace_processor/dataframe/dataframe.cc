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

#include "src/trace_processor/dataframe/dataframe.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/dataframe/cursor.h"
#include "src/trace_processor/dataframe/impl/query_plan.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/value_fetcher.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor::dataframe {

base::StatusOr<Dataframe::QueryPlan> Dataframe::PlanQuery(
    std::vector<FilterSpec>& filter_specs,
    const std::vector<DistinctSpec>& distinct_specs,
    const std::vector<SortSpec>& sort_specs,
    const LimitSpec& limit_spec,
    uint64_t cols_used) const {
  ASSIGN_OR_RETURN(auto plan,
                   impl::QueryPlanBuilder::Build(
                       row_count_, columns_, filter_specs, distinct_specs,
                       sort_specs, limit_spec, cols_used));
  return QueryPlan(std::move(plan));
}

base::StatusOr<Dataframe::Index> Dataframe::BuildIndex(
    const uint32_t* columns_start,
    const uint32_t* columns_end) const {
  std::vector<uint32_t> cols(columns_start, columns_end);
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts;
  sorts.reserve(cols.size());
  for (const auto& col : cols) {
    sorts.push_back(SortSpec{col, SortDirection::kAscending});
  }
  ASSIGN_OR_RETURN(auto plan, PlanQuery(filters, {}, sorts, {}, 0));

  // Heap allocate to avoid potential stack overflows due to large cursor
  // object.
  auto c = std::make_unique<std::optional<Cursor<ErrorValueFetcher>>>();
  PrepareCursor(std::move(plan), *c);
  ErrorValueFetcher vf{};
  c->value().Execute(vf);

  std::vector<uint32_t> permutation;
  permutation.reserve(row_count_);
  for (; !c->value().Eof(); c->value().Next()) {
    permutation.push_back(c->value().RowIndex());
  }
  return Index(std::move(cols),
               std::make_shared<std::vector<uint32_t>>(std::move(permutation)));
}

void Dataframe::AddIndex(Index index) {
  indexes_.emplace_back(std::move(index));
}

void Dataframe::RemoveIndexAt(uint32_t index) {
  indexes_.erase(indexes_.begin() + static_cast<std::ptrdiff_t>(index));
}

dataframe::Dataframe Dataframe::Copy() const {
  return *this;
}

}  // namespace perfetto::trace_processor::dataframe
