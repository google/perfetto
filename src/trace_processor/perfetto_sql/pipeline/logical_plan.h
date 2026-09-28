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

#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/schema.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/util/flex_vector.h"
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

// A value only known when the plan runs: the `index`-th of the values the plan
// is passed, as SQLite passes the value of a constraint on a pipeline's
// output. Bound to that value when the plan is loaded, before anything runs.
struct FilterParam {
  uint32_t index = 0;
};

// A value a filter compares a column with: written in the pipeline, or passed
// to it.
using FilterValue = std::variant<int64_t, double, std::string, FilterParam>;

// One condition a row must meet: `column op values`. The operator is =, !=,
// <, <=, >, >= with one value, IS NULL or IS NOT NULL with none, or IN with
// any number. As in SQL, a comparison is never true of a null.
struct FilterCondition {
  ColumnId column = 0;
  core::Op op{core::Eq{}};
  std::vector<FilterValue> values;
};

// A dataframe read directly. Defined outside Scan because GCC only treats a
// nested class's member initializers as parsed once the enclosing class is.
struct ScanDataframe {
  std::string name;
  // Resolved at compile time; only the selected columns are retained.
  std::vector<std::shared_ptr<const dataframe::Column>> columns;
  uint32_t row_count = 0;
  // The rows the scan's filters keep, in order, once the dataframe has run
  // them; null while they have not been run, or if there are none.
  std::shared_ptr<const core::FlexVector<uint32_t>> rows;
  // The dataframe itself, when its filters compare with parameters and so run
  // afresh each time the plan runs. It lives as long as the statement which
  // runs the plan.
  const dataframe::Dataframe* dataframe = nullptr;
};

// The dataframe the plan is passed as its `index`-th argument when it runs.
// SQLite builds it from a relation the plan read from SQL.
struct ScanDataframeArg {
  uint32_t index = 0;
};

// Reads the rows of a source. Always the first op.
struct Scan {
  using Dataframe = ScanDataframe;
  using DataframeArg = ScanDataframeArg;
  // Where a scan reads from: a dataframe, SQL, or a dataframe argument. SQL
  // sources become dataframe arguments when the plan is written into SQL, and
  // those are bound to dataframes when it is loaded.
  using Source = std::variant<Dataframe, SqlSource, DataframeArg>;
  Source source;
  // Bindings in source column order.
  std::vector<NamedColumn> columns;
  // Conditions on the scan's own columns, every one of which a row must meet
  // to be read. Pushed down from filters above, so the source can run them.
  std::vector<FilterCondition> filters;
};

// `|> WHERE cond AND ...`: keeps the rows every condition holds for. The row
// is otherwise unchanged.
struct Filter {
  std::vector<FilterCondition> conditions;
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
    // The columns passed on to the rows out, in the operand's order. The
    // rest are read only to find the regions.
    std::vector<ColumnId> carried;
  };
  std::vector<Operand> operands;
  // The region's own bounds, which no operand owns.
  ColumnId ts = 0;
  ColumnId dur = 0;
};

}  // namespace op

// The operator a plan node holds. Passes switch on `op.index()` with one case
// per operator, using base::variant_index<Op, T>().
using Op = std::
    variant<op::Scan, op::Filter, op::TreeAccumulate, op::IntervalIntersect>;

// Stable within a plan.
using PlanNodeId = uint32_t;

// An operator together with the relations it reads. A Scan reads none; a
// single-input stage reads one; an intersection reads one per operand.
struct PlanNode {
  Op op;
  std::vector<PlanNodeId> children;

  template <typename T>
  bool Is() const {
    return std::holds_alternative<T>(op);
  }

  // Returns the operator as a T. The node must hold one.
  template <typename T>
  T& Cast() {
    return base::unchecked_get<T>(op);
  }
  template <typename T>
  const T& Cast() const {
    return base::unchecked_get<T>(op);
  }
};

// A tree of operators. Column types are stored once, indexed by ID; operators
// name the values they consume and produce, independently of layout.
//
// The root is the last stage of the pipeline and the leaves are its sources.
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
