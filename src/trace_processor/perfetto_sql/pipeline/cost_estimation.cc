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

#include "src/trace_processor/perfetto_sql/pipeline/cost_estimation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/common/duplicate_types.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/row_estimate.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {
namespace {

// Rows assumed for a relation whose size is unknown until the query runs.
constexpr uint32_t kAssumedRows = 100000;

// Rough relative costs, per row, of the work operators do, in the scale the
// dataframe's own operations are costed in (see the kCost of each bytecode):
// reading and testing a value is a few units, sorting a few more per
// comparison.
constexpr double kFilterCostPerRow = 5;
constexpr double kSortCostPerComparison = 10;
constexpr double kFoldCostPerRow = 10;
constexpr double kSweepCostPerRow = 10;

// A row count, saturating rather than wrapping.
uint32_t Rows(double rows) {
  return static_cast<uint32_t>(std::min(
      rows, static_cast<double>(std::numeric_limits<uint32_t>::max())));
}

double SortCost(uint32_t rows) {
  return rows < 2 ? 0 : kSortCostPerComparison * rows * std::log2(rows);
}

// Applies `conditions` to `rows` knowing nothing of the columns' values, as
// the dataframe planner does for a column it has no statistics on.
core::RowEstimate ApplyBlind(core::RowEstimate rows,
                             const std::vector<op::FilterCondition>& conditions,
                             double& cost) {
  core::RowModel model(rows.estimated);
  for (const op::FilterCondition& condition : conditions) {
    cost += kFilterCostPerRow * model.rows().estimated;
    if (condition.op.Is<core::Eq>()) {
      model.ApplyEqualityFilter(core::HasDuplicates{}, 0);
    } else if (condition.op.Is<core::In>()) {
      model.ApplyInFilter(core::HasDuplicates{}, 0);
    } else {
      model.ApplyNonEqualityFilter();
    }
  }
  return {rows.max, model.rows().estimated};
}

class Estimator {
 public:
  Estimator(const LogicalPlan& plan, const Catalog& catalog)
      : plan_(plan), catalog_(catalog) {}

  core::RowEstimate Estimate(PlanNodeId id) {
    const PlanNode& node = plan_.nodes[id];
    switch (node.op.index()) {
      case base::variant_index<Op, op::Scan>():
        return EstimateScan(node.Cast<op::Scan>());
      case base::variant_index<Op, op::Filter>():
        return ApplyBlind(Estimate(node.children[0]),
                          node.Cast<op::Filter>().conditions, cost_);
      case base::variant_index<Op, op::TreeAccumulate>(): {
        // Numbering and ordering the tree sorts it, then each fold is a pass.
        core::RowEstimate in = Estimate(node.children[0]);
        cost_ += SortCost(in.estimated) + kFoldCostPerRow * in.estimated;
        return in;
      }
      case base::variant_index<Op, op::IntervalIntersect>(): {
        // Each operand is sorted, then swept together. A sweep produces
        // about one region per row it passes, though regions can multiply
        // where operands overlap themselves.
        double estimated = 0;
        double max = 1;
        for (PlanNodeId child : node.children) {
          core::RowEstimate in = Estimate(child);
          cost_ += SortCost(in.estimated) + kSweepCostPerRow * in.estimated;
          estimated += in.estimated;
          max *= std::max<uint32_t>(in.max, 1);
        }
        return {Rows(max), Rows(estimated)};
      }
      default:
        PERFETTO_FATAL("Unknown operator");
    }
  }

  double cost() const { return cost_; }

 private:
  core::RowEstimate EstimateScan(const op::Scan& scan) {
    const auto* source = std::get_if<op::Scan::Dataframe>(&scan.source);
    const dataframe::Dataframe* dataframe =
        source ? catalog_.FindDataframe(source->name) : nullptr;
    if (!dataframe) {
      return ApplyBlind({kAssumedRows, kAssumedRows}, scan.filters, cost_);
    }
    // The same query on the dataframe, planned but not run.
    std::vector<dataframe::FilterSpec> specs;
    const std::vector<std::string>& names = dataframe->column_names();
    for (uint32_t i = 0; i < scan.filters.size(); ++i) {
      const op::FilterCondition& condition = scan.filters[i];
      for (const NamedColumn& column : scan.columns) {
        if (column.id != condition.column) {
          continue;
        }
        auto it = std::find(names.begin(), names.end(), column.name);
        if (it != names.end()) {
          specs.push_back({static_cast<uint32_t>(it - names.begin()), i,
                           condition.op, std::nullopt});
        }
      }
    }
    auto query = dataframe->PlanQuery(specs, {}, {}, {}, 0);
    if (!query.ok()) {
      return ApplyBlind({dataframe->row_count(), dataframe->row_count()},
                        scan.filters, cost_);
    }
    cost_ += query->estimated_cost();
    return {query->max_row_count(), query->estimated_row_count()};
  }

  const LogicalPlan& plan_;
  const Catalog& catalog_;
  double cost_ = 0;
};

}  // namespace

PlanEstimate EstimatePlan(const LogicalPlan& plan, const Catalog& catalog) {
  Estimator estimator(plan, catalog);
  PlanEstimate estimate;
  estimate.rows = estimator.Estimate(plan.root);
  estimate.cost = estimator.cost();
  return estimate;
}

}  // namespace perfetto::trace_processor::pipeline
