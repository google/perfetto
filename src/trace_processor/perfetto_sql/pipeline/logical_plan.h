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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "src/trace_processor/core/common/schema.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/sqlite/sql_source.h"

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

// Reads all rows of a source. Always the first op.
struct Scan {
  using Dataframe = ScanDataframe;
  // A direct dataframe scan or `SELECT * FROM <clause>` executed by SQLite.
  std::variant<Dataframe, SqlSource> source;
  // Bindings in source column order.
  std::vector<NamedColumn> columns;
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

// `INTERVAL INTERSECTION OF (rel AS a, ...) [PER cols]`. A source: the rows
// are the regions every operand covers, not the rows of any one of them.
struct IntervalIntersect {
  // The columns read from one operand, which is a child of the node. Operands
  // are in child order, so `operands[i]` describes `children[i]`.
  struct Operand {
    ColumnId ts = 0;
    ColumnId dur = 0;
    // One per PER column, in the order they were written.
    std::vector<ColumnId> keys;
  };
  std::vector<Operand> operands;
  // The region's own bounds, which no operand owns.
  ColumnId ts = 0;
  ColumnId dur = 0;
};

}  // namespace op

using Op = std::variant<op::Scan, op::TreeAccumulate, op::IntervalIntersect>;

// Which operator a node holds, in the order of Op's alternatives. A pass over
// the plan switches on this with no default case, so an operator added to Op
// fails to compile until every pass decides what to do with it.
enum class OpKind : uint8_t {
  kScan,
  kTreeAccumulate,
  kIntervalIntersect,
};
static_assert(std::variant_size_v<Op> == 3,
              "An operator added to Op must be added to OpKind too");
template <OpKind kind, typename T>
inline constexpr bool kOpKindIs =
    std::is_same_v<std::variant_alternative_t<static_cast<size_t>(kind), Op>,
                   T>;
static_assert(kOpKindIs<OpKind::kScan, op::Scan>);
static_assert(kOpKindIs<OpKind::kTreeAccumulate, op::TreeAccumulate>);
static_assert(kOpKindIs<OpKind::kIntervalIntersect, op::IntervalIntersect>);

// Stable within a plan.
using PlanNodeId = uint32_t;

// An operator together with the relations it reads. A Scan reads none; a
// single-input stage reads one; an intersection reads one per operand.
struct PlanNode {
  Op op;
  std::vector<PlanNodeId> children;

  OpKind kind() const { return static_cast<OpKind>(op.index()); }

  // The operator, which must be a T.
  template <typename T>
  T& Cast() {
    return std::get<T>(op);
  }
  template <typename T>
  const T& Cast() const {
    return std::get<T>(op);
  }
};

// A tree of operators. Column types are stored once, indexed by ID; operators
// name the values they consume and produce, independently of layout.
//
// The root is the last stage of the pipeline and the leaves are its sources,
// so the tree reads the other way up from the SQL: `FROM t |> A |> B` is B at
// the root, reading A, reading a Scan of t.
struct LogicalPlan {
  // Defining SQL names are stable diagnostic labels, independent of aliases
  // in output bindings. Physical temporaries do not get SQL names or IDs.
  std::vector<ColumnSchema> columns;
  std::vector<PlanNode> nodes;
  // The node whose rows are the plan's rows. Every other node reaches it.
  PlanNodeId root = 0;
  // Visible result columns, in order. Internal columns have no binding here.
  std::vector<NamedColumn> output;

  ColumnId AddColumn(std::string name, std::optional<core::StorageType> type) {
    auto id = static_cast<ColumnId>(columns.size());
    columns.push_back({std::move(name), type});
    return id;
  }

  // Adds a node reading `children` and makes it the root, which holds while a
  // plan is built bottom up: each node added is the topmost one so far.
  PlanNodeId AddNode(Op op, std::vector<PlanNodeId> children = {}) {
    auto id = static_cast<PlanNodeId>(nodes.size());
    nodes.push_back({std::move(op), std::move(children)});
    root = id;
    return id;
  }
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LOGICAL_PLAN_H_
