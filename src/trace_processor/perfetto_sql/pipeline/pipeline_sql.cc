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

#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/common/schema.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {
namespace {

using core::StorageType;

std::string QuoteIdentifier(const std::string& name) {
  return "\"" + base::ReplaceAll(name, "\"", "\"\"") + "\"";
}

std::string QuoteString(const std::string& text) {
  return "'" + base::ReplaceAll(text, "'", "''") + "'";
}

// One letter for each type a collected column can hold; `v` carries its type
// per row.
char TypeCode(const std::optional<StorageType>& type) {
  if (!type) {
    return 'v';
  }
  switch (type->index()) {
    case StorageType::GetTypeIndex<core::Id>():
    case StorageType::GetTypeIndex<core::Uint32>():
      // An Id has no storage of its own, so it is collected as a Uint32.
      return 'u';
    case StorageType::GetTypeIndex<core::Int32>():
      return 'i';
    case StorageType::GetTypeIndex<core::Int64>():
      return 'l';
    case StorageType::GetTypeIndex<core::Double>():
      return 'd';
    case StorageType::GetTypeIndex<core::String>():
      return 's';
    default:
      PERFETTO_FATAL("Unknown type");
  }
}

std::optional<std::optional<StorageType>> FromTypeCode(char code) {
  switch (code) {
    case 'v':
      return std::optional<StorageType>();
    case 'u':
      return StorageType{core::Uint32{}};
    case 'i':
      return StorageType{core::Int32{}};
    case 'l':
      return StorageType{core::Int64{}};
    case 'd':
      return StorageType{core::Double{}};
    case 's':
      return StorageType{core::String{}};
    default:
      return std::nullopt;
  }
}

}  // namespace

PlanWithInputs MoveSqlSourcesToInputs(LogicalPlan plan) {
  PlanWithInputs out;
  for (PlanNode& node : plan.nodes) {
    if (!node.Is<op::Scan>()) {
      continue;
    }
    auto& scan = node.Cast<op::Scan>();
    if (!std::holds_alternative<SqlSource>(scan.source)) {
      continue;
    }
    core::Schema columns;
    std::vector<std::string> references;
    for (const NamedColumn& column : scan.columns) {
      columns.push_back({column.name, plan.columns[column.id].type});
      references.push_back(QuoteIdentifier(column.name));
    }
    std::string from = base::unchecked_get<SqlSource>(scan.source).sql();
    out.inputs.push_back("SELECT " + std::string(kCollectFunction) + "(" +
                         QuoteString(WriteCollectedColumns(columns)) + ", " +
                         base::Join(references, ", ") + ") FROM " + from);
    scan.source = op::Scan::Input{static_cast<uint32_t>(out.inputs.size() - 1)};
  }
  out.plan = std::move(plan);
  return out;
}

base::StatusOr<std::string> SelectPipelineSql(const LogicalPlan& plan) {
  if (plan.output.size() > kMaxPipelineColumns) {
    return base::ErrStatus("A pipeline can output at most %u columns, not %zu",
                           kMaxPipelineColumns, plan.output.size());
  }
  PlanWithInputs moved = MoveSqlSourcesToInputs(plan);
  if (moved.inputs.size() > kMaxPipelineInputs) {
    return base::ErrStatus(
        "A pipeline can read at most %u relations from SQL, not %zu",
        kMaxPipelineInputs, moved.inputs.size());
  }
  std::vector<std::string> columns;
  for (uint32_t i = 0; i < plan.output.size(); ++i) {
    columns.push_back("c" + std::to_string(i) + " AS " +
                      QuoteIdentifier(plan.output[i].name));
  }
  std::vector<std::string> arguments{
      "X'" + base::ToHex(SerializePlan(moved.plan)) + "'"};
  for (const std::string& input : moved.inputs) {
    arguments.push_back("(" + input + ")");
  }
  return "SELECT " + base::Join(columns, ", ") + " FROM " + kPipelineFunction +
         "(" + base::Join(arguments, ", ") + ")";
}

std::string WriteCollectedColumns(const core::Schema& columns) {
  std::vector<std::string> out;
  for (const core::ColumnSchema& column : columns) {
    out.push_back(std::string(1, TypeCode(column.type)) + ":" + column.name);
  }
  return base::Join(out, ",");
}

base::StatusOr<core::Schema> ReadCollectedColumns(std::string_view text) {
  core::Schema columns;
  for (const std::string& part : base::SplitString(std::string(text), ",")) {
    std::optional<std::optional<StorageType>> type =
        part.size() >= 2 && part[1] == ':' ? FromTypeCode(part[0])
                                           : std::nullopt;
    if (!type) {
      return base::ErrStatus("%s: malformed columns", kCollectFunction);
    }
    columns.push_back({part.substr(2), *type});
  }
  return columns;
}

}  // namespace perfetto::trace_processor::pipeline
