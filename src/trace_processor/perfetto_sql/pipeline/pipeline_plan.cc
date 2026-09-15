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

#include "src/trace_processor/perfetto_sql/pipeline/pipeline_plan.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/assert_type.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/tree_accumulate.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"
#include "src/trace_processor/perfetto_sql/exec/sql_scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_syntax.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {

namespace ex = core::exec;

// Builds a plan one stage at a time.
//
// Operators are gathered until something has to see all of its input first;
// then they are closed into a pipeline and the breaker reads from that.
class Planner {
 public:
  explicit Planner(const PlanEnvironment& env)
      : env_(env), plan_(std::make_unique<PipelinePlan>()) {}

  base::Status PlanSource(const PipelineSyntax&);
  base::Status PlanStage(const Stage&);
  std::unique_ptr<PipelinePlan> Finish();

 private:
  struct Column {
    std::string name;
    // Columns the planner adds for itself are not output.
    bool visible;
  };
  enum class Order { kAny, kChildFirst, kParentFirst };

  base::Status PlanTreeAccumulate(const TreeAccumulate&, const Stage&);
  base::StatusOr<uint32_t> Resolve(const std::string& name,
                                   const SqlSource& where,
                                   const char* op) const;
  uint32_t AddColumn(std::string name, bool visible) {
    columns_.push_back({std::move(name), visible});
    return static_cast<uint32_t>(columns_.size() - 1);
  }
  // Closes the pending operators into a pipeline, so a breaker can read them.
  void Flush();

  const PlanEnvironment& env_;
  std::unique_ptr<PipelinePlan> plan_;
  std::vector<std::unique_ptr<ex::Operator>> pending_;
  std::vector<Column> columns_;

  // The node number columns, once a stage has asked for them. A later tree
  // stage reuses them: none of them changes which row is which node.
  std::optional<uint32_t> node_column_;
  std::optional<uint32_t> parent_column_;
  Order order_ = Order::kAny;
};

base::Status Planner::PlanSource(const PipelineSyntax& syntax) {
  std::shared_ptr<const dataframe::Dataframe> dataframe =
      syntax.from_name && env_.find_dataframe
          ? env_.find_dataframe(*syntax.from_name)
          : nullptr;
  if (dataframe && dataframe->finalized()) {
    std::vector<uint32_t> indices;
    const std::vector<std::string>& names = dataframe->column_names();
    for (uint32_t i = 0; i < names.size(); ++i) {
      // Hidden from SQLite, so hidden from SELECT * too.
      if (names[i] == "_auto_id") {
        continue;
      }
      indices.push_back(i);
      AddColumn(names[i], true);
    }
    plan_->nodes_.push_back(
        std::make_unique<ex::DataframeScan>(*dataframe, std::move(indices)));
    plan_->dataframes_.push_back(std::move(dataframe));
    return base::OkStatus();
  }

  SqlSource sql = syntax.from.RewriteAllIgnoreExisting(
      SqlSource::FromTraceProcessorImplementation("SELECT * FROM " +
                                                  syntax.from.sql()));
  ASSIGN_OR_RETURN(std::unique_ptr<exec::SqlScan> scan,
                   exec::SqlScan::Create(env_.connection, std::move(sql),
                                         env_.pool, *env_.catalog));
  for (const std::string& name : scan->column_names()) {
    AddColumn(name, true);
  }
  plan_->nodes_.push_back(std::move(scan));
  return base::OkStatus();
}

base::Status Planner::PlanStage(const Stage& stage) {
  if (const auto* op = std::get_if<TreeAccumulate>(&stage.op)) {
    return PlanTreeAccumulate(*op, stage);
  }
  PERFETTO_FATAL("Unknown stage");
}

base::StatusOr<uint32_t> Planner::Resolve(const std::string& name,
                                          const SqlSource& where,
                                          const char* op) const {
  std::optional<uint32_t> found;
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    if (!columns_[i].visible ||
        !base::CaseInsensitiveEqual(columns_[i].name, name)) {
      continue;
    }
    if (found) {
      return base::ErrStatus("%s%s: column '%s' is ambiguous",
                             where.AsTraceback(0).c_str(), op, name.c_str());
    }
    found = i;
  }
  if (!found) {
    return base::ErrStatus("%s%s: no such column: '%s'",
                           where.AsTraceback(0).c_str(), op, name.c_str());
  }
  return *found;
}

base::Status Planner::PlanTreeAccumulate(const TreeAccumulate& op,
                                         const Stage& stage) {
  constexpr char kOp[] = "TREE ACCUMULATE";
  bool up = op.direction == TreeAccumulate::Direction::kUp;

  std::vector<uint32_t> values;
  std::vector<std::string> names;
  for (const TreeAccumulate::Aggregate& agg : op.aggregates) {
    if (agg.function != "SUM" || !agg.column) {
      return base::ErrStatus(
          "%s%s: only SUM(column) is supported so far, not %s",
          agg.sql.AsTraceback(0).c_str(), kOp, agg.function.c_str());
    }
    ASSIGN_OR_RETURN(uint32_t value, Resolve(*agg.column, agg.sql, kOp));
    for (const Column& column : columns_) {
      if (column.visible && base::CaseInsensitiveEqual(column.name, agg.name)) {
        return base::ErrStatus("%s%s: a column named '%s' already exists",
                               agg.sql.AsTraceback(0).c_str(), kOp,
                               agg.name.c_str());
      }
    }
    for (const std::string& name : names) {
      if (base::CaseInsensitiveEqual(name, agg.name)) {
        return base::ErrStatus("%s%s: '%s' is named twice",
                               agg.sql.AsTraceback(0).c_str(), kOp,
                               agg.name.c_str());
      }
    }
    values.push_back(value);
    names.push_back(agg.name);
  }

  if (!node_column_) {
    ASSIGN_OR_RETURN(uint32_t id, Resolve("id", stage.sql, kOp));
    ASSIGN_OR_RETURN(uint32_t parent, Resolve("parent_id", stage.sql, kOp));
    pending_.push_back(std::make_unique<ex::TreeNumberNodes>(id, parent));
    node_column_ = AddColumn("", false);
    parent_column_ = AddColumn("", false);
  }
  for (size_t i = 0; i < values.size(); ++i) {
    pending_.push_back(std::make_unique<ex::AssertType>(
        values[i], ex::AssertTypeTarget{core::Int64{}},
        columns_[values[i]].name));
  }

  if (up && order_ != Order::kChildFirst) {
    Flush();
    plan_->nodes_.push_back(std::make_unique<ex::TreeChildFirst>(
        *plan_->nodes_.back(), *node_column_, *parent_column_));
    order_ = Order::kChildFirst;
  } else if (!up && order_ != Order::kParentFirst) {
    pending_.push_back(
        std::make_unique<ex::TreeParentFirst>(*node_column_, *parent_column_));
    order_ = Order::kParentFirst;
  }

  for (size_t i = 0; i < values.size(); ++i) {
    ex::TreeAccumulateSpec spec{*node_column_, *parent_column_, values[i]};
    if (up) {
      pending_.push_back(std::make_unique<ex::TreeAccumulateUp>(spec));
    } else {
      pending_.push_back(std::make_unique<ex::TreeAccumulateDown>(spec));
    }
    AddColumn(std::move(names[i]), true);
  }
  return base::OkStatus();
}

void Planner::Flush() {
  if (pending_.empty()) {
    return;
  }
  const ex::Source& input = *plan_->nodes_.back();
  plan_->nodes_.push_back(
      std::make_unique<ex::Pipeline>(input, std::move(pending_)));
  pending_.clear();
}

std::unique_ptr<PipelinePlan> Planner::Finish() {
  Flush();
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].visible) {
      plan_->columns_.push_back({std::move(columns_[i].name), i});
    }
  }
  return std::move(plan_);
}

PipelinePlan::PipelinePlan() = default;
PipelinePlan::~PipelinePlan() = default;

base::StatusOr<std::unique_ptr<PipelinePlan>> PlanPipeline(
    const PipelineSyntax& syntax,
    const PlanEnvironment& env) {
  Planner planner(env);
  RETURN_IF_ERROR(planner.PlanSource(syntax));
  for (const Stage& stage : syntax.stages) {
    RETURN_IF_ERROR(planner.PlanStage(stage));
  }
  return planner.Finish();
}

}  // namespace perfetto::trace_processor::pipeline
