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
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/impl/query_plan.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"

namespace perfetto::trace_processor::dataframe {
namespace {

// Creates appropriate storage for a column based on its specification
impl::Storage MakeStorage(const ColumnSpec& c) {
  switch (c.column_type.index()) {
    case ColumnType::GetTypeIndex<Id>():
      return impl::Storage{impl::Storage::Id{}};
    case ColumnType::GetTypeIndex<Uint32>():
      return impl::Storage{impl::Storage::Uint32{}};
    case ColumnType::GetTypeIndex<Int32>():
      return impl::Storage{impl::Storage::Int32{}};
    case ColumnType::GetTypeIndex<Int64>():
      return impl::Storage{impl::Storage::Int64{}};
    case ColumnType::GetTypeIndex<Double>():
      return impl::Storage{impl::Storage::Double{}};
    case ColumnType::GetTypeIndex<String>():
      return impl::Storage{impl::Storage::String{}};
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

// Creates appropriate overlay for a column based on its specification
impl::Overlay MakeOverlay(const ColumnSpec& c) {
  switch (c.nullability.index()) {
    case Nullability::GetTypeIndex<NonNull>():
      return impl::Overlay{impl::Overlay::NoOverlay{}};
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

}  // namespace

Dataframe::Dataframe(const std::vector<ColumnSpec>& column_specs,
                     StringPool* string_pool)
    : string_pool_(string_pool) {
  for (const auto& c : column_specs) {
    columns_.emplace_back(impl::Column{
        c,
        MakeStorage(c),
        MakeOverlay(c),
    });
  }
  base::ignore_result(string_pool_);
}

base::StatusOr<Dataframe::QueryPlan> Dataframe::PlanQuery(
    std::vector<FilterSpec>& specs,
    uint64_t cols_used) {
  if (specs.size() >= impl::kMaxFilters) {
    return base::ErrStatus(
        "Too many filters provided on a single dataframe. We only support up "
        "to 16 filters for performance reasons.");
  }
  return QueryPlan(
      impl::QueryPlanBuilder::Build(row_count_, columns_, specs, cols_used));
}

}  // namespace perfetto::trace_processor::dataframe
