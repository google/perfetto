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

#include "src/trace_processor/core/exec/dataframe_query_scan.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/typed_cursor.h"
#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using CursorValue = dataframe::TypedCursor::FilterValue;

std::vector<dataframe::FilterSpec> Specs(
    const std::vector<Filter::Condition>& conditions) {
  std::vector<dataframe::FilterSpec> specs;
  for (uint32_t i = 0; i < conditions.size(); ++i) {
    specs.push_back({conditions[i].column, i, conditions[i].op, std::nullopt});
  }
  return specs;
}

// Sets `cursor`'s filter values for a run from `conditions`, storing them in
// `values`, which the cursor reads in place. False if a condition holds for no
// row.
bool SetValues(dataframe::TypedCursor& cursor,
               const std::vector<Filter::Condition>& conditions,
               const Filter::Params& params,
               std::vector<std::vector<CursorValue>>& values) {
  for (uint32_t i = 0; i < conditions.size(); ++i) {
    const Filter::Condition& condition = conditions[i];
    const std::vector<Filter::Value>* source = &condition.values;
    if (condition.param) {
      const Filter::Param* param = *condition.param < params.size()
                                       ? &params[*condition.param]
                                       : nullptr;
      // No comparison is true of a null or of nothing.
      if (!param || !*param || (*param)->empty()) {
        return false;
      }
      source = &**param;
    }
    std::vector<CursorValue>& v = values[i];
    v.clear();
    for (const Filter::Value& value : *source) {
      switch (value.index()) {
        case base::variant_index<Filter::Value, int64_t>():
          v.emplace_back(base::unchecked_get<int64_t>(value));
          break;
        case base::variant_index<Filter::Value, double>():
          v.emplace_back(base::unchecked_get<double>(value));
          break;
        case base::variant_index<Filter::Value, std::string>():
          v.emplace_back(base::unchecked_get<std::string>(value).c_str());
          break;
        default:
          PERFETTO_FATAL("Unknown filter value");
      }
    }
    if (condition.op.Is<In>()) {
      cursor.SetFilterValueListUnchecked(i, v.data(),
                                         static_cast<uint32_t>(v.size()));
      continue;
    }
    if (v.empty()) {
      continue;
    }
    switch (v[0].index()) {
      case base::variant_index<CursorValue, int64_t>():
        cursor.SetFilterValueUnchecked(i, base::unchecked_get<int64_t>(v[0]));
        break;
      case base::variant_index<CursorValue, double>():
        cursor.SetFilterValueUnchecked(i, base::unchecked_get<double>(v[0]));
        break;
      case base::variant_index<CursorValue, const char*>():
        cursor.SetFilterValueUnchecked(i,
                                       base::unchecked_get<const char*>(v[0]));
        break;
      default:
        PERFETTO_FATAL("Unknown filter value");
    }
  }
  return true;
}

// Appends the rows `cursor` finds to `rows`, in increasing order.
void CollectRows(dataframe::TypedCursor& cursor, FlexVector<uint32_t>& rows) {
  for (cursor.ExecuteUnchecked(); !cursor.Eof(); cursor.Next()) {
    rows.push_back(cursor.RowIndex());
  }
  // A scan's expanders walk a sparse column forward, so rows must increase,
  // but an index lists them in its own order.
  // TODO(lalitm): ask the dataframe for rows in storage order instead, so it
  // can skip the sort when no index reordered them.
  std::sort(rows.begin(), rows.end());
}

}  // namespace

struct DataframeQueryScan::RowsState : OperatorState {
  RowsState(const dataframe::Dataframe* dataframe,
            std::vector<dataframe::FilterSpec> specs,
            size_t conditions)
      : cursor(dataframe, std::move(specs), {}), values(conditions) {}
  ~RowsState() override;

  // Planned once: planning is most of what a query costs.
  dataframe::TypedCursor cursor;
  // By condition.
  std::vector<std::vector<CursorValue>> values;
  BufferPool<FlexVector<uint32_t>> rows;
};

DataframeQueryScan::RowsState::~RowsState() = default;

FlexVector<uint32_t> RunDataframeQuery(
    const dataframe::Dataframe& dataframe,
    const std::vector<Filter::Condition>& conditions,
    const Filter::Params& params) {
  dataframe::TypedCursor cursor(&dataframe, Specs(conditions), {});
  std::vector<std::vector<CursorValue>> values(conditions.size());
  FlexVector<uint32_t> rows;
  if (SetValues(cursor, conditions, params, values)) {
    CollectRows(cursor, rows);
  }
  return rows;
}

DataframeQueryScan::DataframeQueryScan(
    const dataframe::Dataframe* dataframe,
    std::vector<std::shared_ptr<const dataframe::Column>> columns,
    std::vector<Filter::Condition> conditions,
    const Filter::Params* params)
    : DataframeScan(std::move(columns), 0, nullptr),
      dataframe_(dataframe),
      conditions_(std::move(conditions)),
      params_(params) {}

DataframeQueryScan::~DataframeQueryScan() = default;

std::unique_ptr<OperatorState> DataframeQueryScan::MakeRowsState() const {
  return std::make_unique<RowsState>(dataframe_, Specs(conditions_),
                                     conditions_.size());
}

std::shared_ptr<const FlexVector<uint32_t>> DataframeQueryScan::FindRows(
    OperatorState* state) const {
  auto& s = state->Cast<RowsState>();
  std::shared_ptr<FlexVector<uint32_t>> rows = s.rows.Acquire();
  rows->clear();
  if (SetValues(s.cursor, conditions_, *params_, s.values)) {
    CollectRows(s.cursor, *rows);
  }
  return rows;
}

}  // namespace perfetto::trace_processor::core::exec
