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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_TREE_COLUMN_UTILS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_TREE_COLUMN_UTILS_H_

#include <cstdint>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/perfetto_sql/tree/tree.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"

namespace perfetto::trace_processor::plugins::tree {

// Find a column by name in a vector of passthrough columns.
// Returns nullptr if not found.
inline const PassthroughColumn* FindColumnByName(
    const std::vector<PassthroughColumn>& columns,
    const std::string& name) {
  for (const auto& col : columns) {
    if (col.name == name) {
      return &col;
    }
  }
  return nullptr;
}

// Find a column by name, returning an error if not found.
inline base::StatusOr<const PassthroughColumn*> FindColumnOrError(
    const std::vector<PassthroughColumn>& columns,
    const std::string& name,
    const char* context) {
  const PassthroughColumn* col = FindColumnByName(columns, name);
  if (!col) {
    return base::ErrStatus("%s: column '%s' not found", context, name.c_str());
  }
  return col;
}

// Push a SQLite value to a passthrough column.
// Initializes the column type on first non-null value.
// Returns false on type mismatch or unsupported blob type.
inline bool PushSqliteValueToColumn(PassthroughColumn& col,
                                    sqlite3_value* value,
                                    StringPool* pool) {
  switch (sqlite::value::Type(value)) {
    case sqlite::Type::kNull:
      // For null, push a sentinel value based on type.
      // If type not set yet, default to int64.
      if (std::holds_alternative<std::monostate>(col.data)) {
        col.data = std::vector<int64_t>();
      }
      if (col.IsInt64()) {
        col.AsInt64().push_back(kNullInt64);
      } else if (col.IsDouble()) {
        col.AsDouble().push_back(std::numeric_limits<double>::quiet_NaN());
      } else if (col.IsString()) {
        col.AsString().push_back(StringPool::Id::Null());
      }
      break;
    case sqlite::Type::kInteger: {
      int64_t val = sqlite::value::Int64(value);
      if (std::holds_alternative<std::monostate>(col.data)) {
        col.data = std::vector<int64_t>();
      }
      if (PERFETTO_UNLIKELY(!col.IsInt64())) {
        return false;
      }
      col.AsInt64().push_back(val);
      break;
    }
    case sqlite::Type::kFloat: {
      double val = sqlite::value::Double(value);
      if (std::holds_alternative<std::monostate>(col.data)) {
        col.data = std::vector<double>();
      }
      if (PERFETTO_UNLIKELY(!col.IsDouble())) {
        return false;
      }
      col.AsDouble().push_back(val);
      break;
    }
    case sqlite::Type::kText: {
      const char* text = sqlite::value::Text(value);
      if (std::holds_alternative<std::monostate>(col.data)) {
        col.data = std::vector<StringPool::Id>();
      }
      if (PERFETTO_UNLIKELY(!col.IsString())) {
        return false;
      }
      col.AsString().push_back(pool->InternString(text));
      break;
    }
    case sqlite::Type::kBlob:
      return false;
  }
  return true;
}

// Get the AdhocDataframeBuilder column type for a passthrough column.
inline dataframe::AdhocDataframeBuilder::ColumnType GetColumnType(
    const PassthroughColumn& col) {
  using ColType = dataframe::AdhocDataframeBuilder::ColumnType;
  if (col.IsInt64()) {
    return ColType::kInt64;
  }
  if (col.IsDouble()) {
    return ColType::kDouble;
  }
  return ColType::kString;
}

// Get column types for a vector of passthrough columns.
inline std::vector<dataframe::AdhocDataframeBuilder::ColumnType> GetColumnTypes(
    const std::vector<PassthroughColumn>& columns) {
  using ColType = dataframe::AdhocDataframeBuilder::ColumnType;
  std::vector<ColType> types;
  types.reserve(columns.size());
  for (const auto& col : columns) {
    types.push_back(GetColumnType(col));
  }
  return types;
}

// Push all passthrough columns to the dataframe builder.
// Returns the next column index after all columns are pushed.
inline uint32_t PushAllGatheredColumns(
    dataframe::AdhocDataframeBuilder& builder,
    uint32_t start_col_idx,
    const std::vector<PassthroughColumn>& columns,
    base::Span<const uint32_t> source_indices) {
  uint32_t col_idx = start_col_idx;
  for (const auto& col : columns) {
    if (col.IsInt64()) {
      builder.PushGatheredWithSentinelUnchecked(
          col_idx, base::MakeSpan(col.AsInt64()), source_indices, kNullInt64);
    } else if (col.IsDouble()) {
      builder.PushGatheredWithSentinelUnchecked(
          col_idx, base::MakeSpan(col.AsDouble()), source_indices);
    } else if (col.IsString()) {
      builder.PushGatheredWithSentinelUnchecked(
          col_idx, base::MakeSpan(col.AsString()), source_indices);
    }
    col_idx++;
  }
  return col_idx;
}

// Gather a vector's values via source_indices indirection.
template <typename T>
std::vector<T> GatherValues(const std::vector<T>& src,
                            const std::vector<uint32_t>& source_indices) {
  std::vector<T> result(source_indices.size());
  for (uint32_t i = 0; i < source_indices.size(); ++i) {
    result[i] = src[source_indices[i]];
  }
  return result;
}

// Gather a single passthrough column via source_indices indirection.
inline PassthroughColumn GatherPassthroughColumn(
    const PassthroughColumn& col,
    const std::vector<uint32_t>& source_indices) {
  if (col.IsInt64()) {
    return {col.name, GatherValues(col.AsInt64(), source_indices)};
  }
  if (col.IsDouble()) {
    return {col.name, GatherValues(col.AsDouble(), source_indices)};
  }
  if (col.IsString()) {
    return {col.name, GatherValues(col.AsString(), source_indices)};
  }
  return PassthroughColumn(col.name);
}

// Gather all passthrough columns via source_indices indirection.
inline std::vector<PassthroughColumn> GatherAllPassthroughColumns(
    const std::vector<PassthroughColumn>& columns,
    const std::vector<uint32_t>& source_indices) {
  std::vector<PassthroughColumn> result;
  result.reserve(columns.size());
  for (const auto& col : columns) {
    result.push_back(GatherPassthroughColumn(col, source_indices));
  }
  return result;
}

}  // namespace perfetto::trace_processor::plugins::tree

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_TREE_COLUMN_UTILS_H_
