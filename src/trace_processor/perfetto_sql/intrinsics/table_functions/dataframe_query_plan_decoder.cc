/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/dataframe_query_plan_decoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/dataframe.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/tables_py.h"

namespace perfetto::trace_processor {
namespace tables {

DataframeQueryPlanDecoderTable::~DataframeQueryPlanDecoderTable() = default;

}  // namespace tables

DataframeQueryPlanDecoder::DataframeQueryPlanDecoder(StringPool* pool)
    : string_pool_(pool) {}

base::StatusOr<std::unique_ptr<Table>> DataframeQueryPlanDecoder::ComputeTable(
    const std::vector<SqlValue>& arguments) {
  PERFETTO_CHECK(arguments.size() == 1);
  if (arguments[0].type != SqlValue::kString) {
    return base::ErrStatus(
        "__intrinsic_dataframe_query_plan_decoder takes the serialized query "
        "plan as a string.");
  }

  const std::string& serialized_query_plan = arguments[0].AsString();
  auto table =
      std::make_unique<tables::DataframeQueryPlanDecoderTable>(string_pool_);

  auto plan =
      dataframe::Dataframe::QueryPlan::Deserialize(serialized_query_plan);
  for (const auto& bc : plan.BytecodeToString()) {
    table->Insert(tables::DataframeQueryPlanDecoderTable::Row(
        string_pool_->InternString(base::StringView(bc))));
  }
  return std::unique_ptr<Table>(std::move(table));
}

Table::Schema DataframeQueryPlanDecoder::CreateSchema() {
  return tables::DataframeQueryPlanDecoderTable::ComputeStaticSchema();
}

std::string DataframeQueryPlanDecoder::TableName() {
  return tables::DataframeQueryPlanDecoderTable::Name();
}

uint32_t DataframeQueryPlanDecoder::EstimateRowCount() {
  return 20;
}

}  // namespace perfetto::trace_processor
