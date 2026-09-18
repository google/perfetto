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

#include "src/trace_processor/perfetto_sql/pipeline/logical_plan_test_utils.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/common/storage_types.h"

namespace perfetto::trace_processor::pipeline {
namespace {

const char* TypeName(core::StorageType type) {
  switch (type.index()) {
    case core::StorageType::GetTypeIndex<core::Id>():
      return "id";
    case core::StorageType::GetTypeIndex<core::Uint32>():
      return "uint32";
    case core::StorageType::GetTypeIndex<core::Int32>():
      return "int32";
    case core::StorageType::GetTypeIndex<core::Int64>():
      return "int64";
    case core::StorageType::GetTypeIndex<core::Double>():
      return "double";
    case core::StorageType::GetTypeIndex<core::String>():
      return "string";
    default:
      PERFETTO_FATAL("Unknown storage type");
  }
}

std::string ColumnString(const LogicalPlan& plan, ColumnId id) {
  std::string out = "#" + std::to_string(id);
  if (plan.columns[id].type) {
    out += ":";
    out += TypeName(*plan.columns[id].type);
  }
  return out;
}

std::string OpString(const LogicalPlan& plan, const Op& op) {
  struct Visitor {
    const LogicalPlan& plan;
    std::string operator()(const op::Scan& scan) const {
      std::string out = "Scan(";
      if (const auto* dataframe =
              std::get_if<op::Scan::Dataframe>(&scan.source)) {
        out += "table " + dataframe->name;
      } else {
        out += "sql " + std::get<SqlSource>(scan.source).sql();
      }
      out += ") [";
      for (size_t i = 0; i < scan.columns.size(); ++i) {
        if (i)
          out += ", ";
        out += ColumnString(plan, scan.columns[i].id) + " AS " +
               scan.columns[i].name;
      }
      return out + "]";
    }
    std::string operator()(const op::TreeAccumulate& acc) const {
      std::string out = "TreeAccumulate(";
      out += acc.direction == op::TreeDirection::kUp ? "up" : "down";
      out += ", node=#" + std::to_string(acc.node_column);
      out += ", parent=#" + std::to_string(acc.parent_column);
      for (const auto& agg : acc.aggregates) {
        PERFETTO_DCHECK(agg.function == op::TreeAccumulate::Function::kSum);
        out += ", SUM(#" + std::to_string(agg.column) + ") -> " +
               ColumnString(plan, agg.output);
      }
      return out + ")";
    }
  };
  return std::visit(Visitor{plan}, op);
}

}  // namespace

std::string LogicalPlanToString(const LogicalPlan& plan) {
  std::string out;
  for (const Op& op : plan.ops) {
    out += OpString(plan, op) + "\n";
  }
  out += "Output(";
  for (size_t i = 0; i < plan.output.size(); ++i) {
    if (i)
      out += ", ";
    out +=
        "#" + std::to_string(plan.output[i].id) + " AS " + plan.output[i].name;
  }
  return out + ")\n";
}

}  // namespace perfetto::trace_processor::pipeline
