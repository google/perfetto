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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OUTPUT_PROPERTIES_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OUTPUT_PROPERTIES_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/expression.h"
#include "src/trace_processor/core/exec/param_value.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

struct BoundPlan;
struct LoweringContext;

// What is known about the batches a node outputs: their columns and types, how
// the rows are arranged, the tree they are numbered as and the group number
// columns they carry. Rewrites read this to decide, and PlanNodeInputs compares
// it with what each node needs.
struct OutputProperties {
  // A layout the rows are known to have.
  struct Arrangement {
    // The columns the rows are grouped by. Empty means one group.
    std::vector<ColumnId> grouped_by;

    // The keys each group is sorted by.
    std::vector<SortKey> sorted_by;
  };

  // The tree the rows are numbered as.
  struct Tree {
    ColumnId node = 0;
    ColumnId parent = 0;
    // The direction the rows walk the tree in, if they do.
    std::optional<TreeDirection> walk;
    // The columns holding the node numbers.
    ColumnId node_number = 0;
    ColumnId parent_number = 0;
  };

  // A column holding group numbers.
  struct GroupNumbers {
    // The columns the groups are formed by.
    std::vector<ColumnId> keys;
    ColumnId column = 0;
  };

  // A column of a batch, and what it holds here.
  struct Column {
    ColumnId id = 0;
    PlanType type;
    // It was bound as holding only nulls, whatever it has since converted to.
    bool all_null = false;
  };

  // The most recent sort, while nothing has read its order, and what was known
  // before it: it can still be undone.
  struct OpenSort {
    PlanNodeId node = 0;
    uint32_t input = 0;
    size_t step = 0;
    std::vector<Arrangement> arrangements;
    std::optional<TreeDirection> walk;
  };

  // The position of `column` in a batch, if it has one.
  std::optional<uint32_t> PositionOf(ColumnId column) const;

  // The type of `column`, which the batches must have.
  const PlanType& TypeOf(ColumnId column) const;

  // Whether the rows are grouped by `keys` and sorted by `sorted_by` within
  // each group. Sorting by some keys first also groups by them. For example,
  // rows sorted by (cpu, ts) are grouped by cpu and sorted by ts in each group.
  bool IsArranged(const std::vector<ColumnId>& keys,
                  const std::vector<SortKey>& sorted_by) const;

  // The longest prefix of a known sort order that uses only `allowed`.
  std::vector<SortKey> SortedBy(const std::vector<ColumnId>& allowed) const;

  // The direction the rows walk the tree of `node` and `parent` in, if known.
  std::optional<TreeDirection> TreeWalk(ColumnId node, ColumnId parent) const;

  // The column numbering the groups of `keys`, if the rows carry one.
  std::optional<ColumnId> GroupNumbersOf(
      const std::vector<ColumnId>& keys) const;

  // Some rows were dropped by a step reading their order. Rows kept stay in
  // order, in batches which can be small, but a tree numbered before may now
  // name parents which are gone.
  void DropRows();

  // The columns of a batch, in order.
  std::vector<Column> columns;

  // Every layout known to hold.
  std::vector<Arrangement> arrangements;

  // The tree the rows are numbered as, if any.
  std::optional<Tree> tree;

  // The group number columns, which stay valid when rows are reordered.
  std::vector<GroupNumbers> group_numbers;

  // Whether batches are known to be full, so no combining is needed.
  bool full_batches = true;

  // Set while a sort can still be undone.
  std::optional<OpenSort> open_sort;
};

// What a node needs of the rows of one of its inputs. All of it is optional,
// and it is met in the order of these fields.
struct InputNeeds {
  // A column to hold a type, which must convert to it without loss.
  struct Type {
    ColumnId column = 0;
    core::StorageType type;
    // Only if the column's type is not yet known.
    bool if_unknown = false;
  };

  std::vector<Type> types_first;

  struct TreeWalk {
    ColumnId node = 0;
    ColumnId parent = 0;
    TreeDirection direction = TreeDirection::kUp;
  };

  // Grouped by `grouped_by` and sorted by `sorted_by` within each group.
  std::optional<OutputProperties::Arrangement> arranged;

  // A column numbering the groups of `arranged`.
  bool group_numbers = false;

  std::optional<TreeWalk> tree_walk;

  std::vector<Type> types_then;

  bool full_batches = false;
};

// What it takes for an input to meet a node's needs, each step run in order
// before the node reads the input.
namespace input_step {
struct Sort {
  std::vector<SortKey> keys;
  bool combine_first = false;
};
struct Group {
  std::vector<ColumnId> keys;
  std::vector<SortKey> sorted_by;
  ColumnId numbers = 0;
  bool combine_first = false;
};
struct NumberTree {
  ColumnId node = 0;
  ColumnId parent = 0;
  ColumnId node_number = 0;
  ColumnId parent_number = 0;
};
struct WalkTree {
  ColumnId node_number = 0;
  ColumnId parent_number = 0;
  TreeDirection direction = TreeDirection::kUp;
  bool combine_first = false;
};
struct CombineBatches {};
struct Convert {
  ColumnId column = 0;
  core::StorageType type;
  // Its values are all null, so it can be converted without reading them.
  bool all_null = false;
};
}  // namespace input_step
using InputStep = std::variant<input_step::Sort,
                               input_step::Group,
                               input_step::NumberTree,
                               input_step::WalkTree,
                               input_step::CombineBatches,
                               input_step::Convert>;

// The value a condition tests with, for a column of storage `type` compared by
// `op`, as planning decides it.
struct PlannedParam {
  enum class State : uint8_t {
    // Not decided yet: the column's type is not known yet, or a rewrite has
    // not planned the condition.
    kUnplanned,
    // The condition takes no value: IS [NOT] NULL.
    kNone,
    // `value`, cast from `literals` when the plan is built. With no literals,
    // it matches no row.
    kConstant,
    // Cast by each run from `literals`, a single bound param.
    kBound,
    // Moved into the physical plan by lowering.
    kTaken,
  };

  State state = State::kUnplanned;
  core::exec::ParamValue value;
  std::vector<Literal> literals;
  core::StorageType type{core::Int64{}};
  core::Op op{core::Eq{}};
};

// One input of a node, once planned.
struct PlannedInput {
  std::vector<InputStep> steps;
  // What is known of the rows after `steps`.
  OutputProperties rows;
};

// What is decided about a node before it is lowered.
struct PlannedNode {
  std::vector<PlannedInput> inputs;
  OutputProperties output;
  // The programs of the expressions it computes, if any.
  std::vector<core::exec::ExprProgram> programs;
  // By condition, the value it tests with.
  std::vector<PlannedParam> params;
  // The columns it added to the plan, which planning it again reuses.
  std::vector<ColumnId> new_columns;
  size_t new_columns_used = 0;
};

// What an operation derives its output from.
class DeriveContext {
 public:
  DeriveContext(BoundPlan* bound,
                PlanNodeId self,
                const LoweringContext& context)
      : bound_(bound), self_(self), context_(context) {}

  const BoundPlan& bound() const { return *bound_; }
  PlanNodeId self() const { return self_; }
  const LoweringContext& context() const { return context_; }

  // What is known of input `i` once its needs are met.
  const OutputProperties& input(uint32_t i) const;

  // `id` as a column of the output: as it is in an input which has it, else as
  // binding found it.
  OutputProperties::Column ColumnOf(ColumnId id) const;

  // A new column for the plan, for a value only the plan's steps read.
  OutputProperties::Column NewColumn();

  // Keeps the programs of the expressions this node computes.
  void SetPrograms(std::vector<core::exec::ExprProgram> programs);

  // The values of the conditions this node tests.
  std::vector<PlannedParam>& params();

  // Records why the plan cannot run.
  void Fail(base::Status status);

 private:
  BoundPlan* bound_;
  PlanNodeId self_;
  const LoweringContext& context_;
};

// What an interval operator needs of its input: Int64 `ts` and `dur`, grouped
// by `keys` with group numbers, and sorted by `ts` within each group.
InputNeeds IntervalLaneNeeds(ColumnId ts,
                             ColumnId dur,
                             const std::vector<ColumnId>& keys);

// A column of one of a node's inputs.
struct InputColumn {
  uint32_t input = 0;
  ColumnId column = 0;
};

// Plans how each input of node `id` meets its needs, and derives its output.
// Everything below `id` must already be planned.
void PlanNodeInputs(BoundPlan* bound,
                    PlanNodeId id,
                    const LoweringContext& context);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OUTPUT_PROPERTIES_H_
