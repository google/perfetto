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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LOGICAL_PLAN_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LOGICAL_PLAN_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "src/trace_processor/core/common/schema.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

namespace perfetto::trace_processor::pipeline {

using core::ColumnSchema;
using core::Schema;

// Stable within a plan. Lowering assigns physical batch positions separately.
using ColumnId = uint32_t;

// A name in a scope or result. Multiple names can refer to the same value.
struct NamedColumn {
  std::string name;
  ColumnId id;
};

namespace op {

// A dataframe read directly. Defined outside Scan because GCC only treats a
// nested class's member initializers as parsed once the enclosing class is.
struct ScanDataframe {
  std::string name;
  // Resolved at compile time; only the selected columns are retained.
  std::vector<std::shared_ptr<const dataframe::Column>> columns;
  uint32_t row_count = 0;
};

// Reads all rows of a source: the first op of a plan, or the operand of a
// later one.
struct Scan {
  using Dataframe = ScanDataframe;
  // A direct dataframe scan or `SELECT * FROM <clause>` executed by SQLite.
  std::variant<Dataframe, SqlSource> source;
  // Bindings in source column order.
  std::vector<NamedColumn> columns;
  // The SQL source's statement, prepared to describe it and good for one run.
  std::shared_ptr<SqliteConnection::PreparedStatement> statement;
};

enum class TreeDirection : uint8_t { kUp, kDown };

// `|> TREE ACCUMULATE UP | DOWN agg AS name, ...`. Appends one column per
// aggregate.
struct TreeAccumulate {
  enum class Function : uint8_t { kSum };
  struct Aggregate {
    Function function = Function::kSum;
    ColumnId column = 0;
    ColumnId output = 0;
  };
  TreeDirection direction = TreeDirection::kUp;
  // The logical id/parent_id columns describing the input tree.
  ColumnId node_column = 0;
  ColumnId parent_column = 0;
  std::vector<Aggregate> aggregates;
};

// `|> [LEFT] INTERVAL JOIN operand AS alias relationship [PER cols]`. Appends
// the operand's columns, once per operand row the input row relates to.
struct IntervalJoin {
  // How an operand interval has to relate to an input interval to be joined.
  enum class Relationship : uint8_t {
    kOverlappingBounds,
    kCoveringBegin,
    kCoveringEnd,
    kCoveringBounds,
    kWithinBounds,
  };
  // The columns of one side holding its intervals and the PER key.
  struct Side {
    ColumnId ts = 0;
    // A side without a dur column is a side of points.
    std::optional<ColumnId> dur;
    // Pairwise equal across the two sides of every joined row.
    std::vector<ColumnId> keys;
  };
  Scan operand;
  Side input_side;
  Side operand_side;
  Relationship relationship = Relationship::kOverlappingBounds;
  // LEFT: an input row which joins with nothing is kept, with a null operand.
  bool keep_unmatched = false;
};

}  // namespace op

using Op = std::variant<op::Scan, op::TreeAccumulate, op::IntervalJoin>;

// A flat chain of operators. Column types are stored once, indexed by ID;
// operators name the values they consume and produce, independently of layout.
struct LogicalPlan {
  // Defining SQL names are stable diagnostic labels, independent of aliases
  // in output bindings. Physical temporaries do not get SQL names or IDs.
  std::vector<ColumnSchema> columns;
  std::vector<Op> ops;
  // Visible result columns, in order. Internal columns have no binding here.
  std::vector<NamedColumn> output;

  ColumnId AddColumn(std::string name, std::optional<core::StorageType> type) {
    auto id = static_cast<ColumnId>(columns.size());
    columns.push_back({std::move(name), type});
    return id;
  }
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LOGICAL_PLAN_H_
