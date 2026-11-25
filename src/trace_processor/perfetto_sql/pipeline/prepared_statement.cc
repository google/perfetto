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

#include "src/trace_processor/perfetto_sql/pipeline/prepared_statement.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace {

base::StatusOr<std::unique_ptr<PhysicalPlan>> BindAndLower(
    LogicalPlan plan,
    bool require_table_stamp,
    const ExecutionContext& ctx,
    LoweringContext::SparseOutputs sparse_outputs,
    LoweringContext::SortedOutputs sorted_outputs) {
  PERFETTO_CHECK(ctx.pool != nullptr);
  if (require_table_stamp && !ctx.plan_key) {
    return base::ErrStatus(
        "Pipeline: stamped statement requires a PlanKey for execution");
  }
  const PlanKey* key = require_table_stamp ? ctx.plan_key : nullptr;
  ASSIGN_OR_RETURN(BoundPlan bound, BindPlan(std::move(plan), ctx.catalog, key,
                                             ctx.args, ctx.pool));
  LoweringContext lowering;
  lowering.pool = ctx.pool;
  lowering.sparse_outputs = sparse_outputs;
  lowering.sorted_outputs = sorted_outputs;
  RETURN_IF_ERROR(RunBoundPasses(&bound, lowering));
  std::unique_ptr<PhysicalPlan> physical = Lower(&bound, lowering);
  RETURN_IF_ERROR(bound.status);
  return std::move(physical);
}

}  // namespace

PreparedStatement::PreparedStatement(std::unique_ptr<LogicalPlan> plan,
                                     bool require_table_stamp)
    : plan_(std::move(plan)), require_table_stamp_(require_table_stamp) {}

PreparedStatement::PreparedStatement(PreparedStatement&&) noexcept = default;
PreparedStatement& PreparedStatement::operator=(PreparedStatement&&) noexcept =
    default;
PreparedStatement::~PreparedStatement() = default;

std::string PreparedStatement::Serialize() const {
  return SerializePlan(*plan_);
}

PreparedStatement PreparedStatement::Deserialize(std::string_view bytes) {
  return PreparedStatement(
      std::make_unique<LogicalPlan>(DeserializePlanUnstamped(bytes)),
      /*require_table_stamp=*/false);
}

std::string PreparedStatement::Serialize(const PlanKey& key) const {
  return SerializePlan(*plan_, &key);
}

base::StatusOr<PreparedStatement> PreparedStatement::Deserialize(
    std::string_view bytes,
    const PlanKey& key) {
  ASSIGN_OR_RETURN(LogicalPlan plan, DeserializePlanBytes(bytes, key));
  return PreparedStatement(std::make_unique<LogicalPlan>(std::move(plan)),
                           /*require_table_stamp=*/true);
}

base::StatusOr<BatchIterator> PreparedStatement::Execute(
    const ExecutionContext& ctx) const& {
  ASSIGN_OR_RETURN(std::unique_ptr<PhysicalPlan> physical,
                   BindAndLower(ClonePlan(*plan_), require_table_stamp_, ctx,
                                LoweringContext::SparseOutputs::kExpand,
                                LoweringContext::SortedOutputs::kCopied));
  return BatchIterator(std::move(physical));
}

base::StatusOr<BatchIterator> PreparedStatement::Execute(
    const ExecutionContext& ctx) && {
  ASSIGN_OR_RETURN(std::unique_ptr<PhysicalPlan> physical,
                   BindAndLower(std::move(*plan_), require_table_stamp_, ctx,
                                LoweringContext::SparseOutputs::kExpand,
                                LoweringContext::SortedOutputs::kCopied));
  return BatchIterator(std::move(physical));
}

base::StatusOr<RowIterator> PreparedStatement::ExecuteRows(
    const ExecutionContext& ctx) const& {
  ASSIGN_OR_RETURN(std::unique_ptr<PhysicalPlan> physical,
                   BindAndLower(ClonePlan(*plan_), require_table_stamp_, ctx,
                                LoweringContext::SparseOutputs::kLend,
                                LoweringContext::SortedOutputs::kInPlace));
  return RowIterator(std::move(physical));
}

base::StatusOr<RowIterator> PreparedStatement::ExecuteRows(
    const ExecutionContext& ctx) && {
  ASSIGN_OR_RETURN(std::unique_ptr<PhysicalPlan> physical,
                   BindAndLower(std::move(*plan_), require_table_stamp_, ctx,
                                LoweringContext::SparseOutputs::kLend,
                                LoweringContext::SortedOutputs::kInPlace));
  return RowIterator(std::move(physical));
}

uint32_t PreparedStatement::column_count() const {
  return static_cast<uint32_t>(plan_->output().size());
}

const std::string& PreparedStatement::column_name(uint32_t index) const {
  return plan_->names().text(plan_->output()[index].name);
}

}  // namespace perfetto::trace_processor::pipeline
