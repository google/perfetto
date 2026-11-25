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

#include "src/trace_processor/perfetto_sql/pipeline/iterator.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/param_value.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"

namespace perfetto::trace_processor::pipeline {
namespace internal {

// Owns batch execution objects behind one heap allocation so BatchIterator can
// use `= default` moves. Destroyed in reverse order: scratch -> context -> plan
// (op_state is arena-owned by context).
struct BatchState {
  explicit BatchState(std::unique_ptr<PhysicalPlan> p) : plan(std::move(p)) {
    context.SetPipelines(&plan->pipelines());
    op_state = plan->source().MakeState(context);
  }

  std::unique_ptr<PhysicalPlan> plan;
  core::exec::Context context;
  core::exec::OperatorState* op_state = nullptr;
  core::exec::RowBatch scratch;
};

// Owns row execution objects behind one heap allocation so RowIterator can use
// `= default` moves. Destroyed in reverse order: cursor -> plan.
struct RowState {
  explicit RowState(std::unique_ptr<PhysicalPlan> p)
      : plan(std::move(p)), cursor(plan->source(), plan->pipelines()) {}

  std::unique_ptr<PhysicalPlan> plan;
  core::exec::RowCursor cursor;
};

ParamBindingLayout DescribeParams(const PhysicalPlan& plan) {
  ParamBindingLayout layout;
  layout.pool = plan.pool_;
  layout.initial_params.resize(plan.param_sources_.size());
  for (uint32_t i = 0; i < plan.param_sources_.size(); ++i) {
    const PhysicalPlan::ParamSource& s = plan.param_sources_[i];
    if (s.state == PlannedParam::State::kBound ||
        s.state == PlannedParam::State::kExternal) {
      layout.specs.push_back(
          BoundParamSpec{s.type, s.op, s.literals[0].bound, i,
                         s.state == PlannedParam::State::kExternal});
    } else if (!s.literals.empty()) {
      layout.initial_params[i] = CastLiterals(
          s.op, s.literals.data(), static_cast<uint32_t>(s.literals.size()),
          s.type, *plan.pool_);
      if (s.op.Is<core::Glob>()) {
        core::exec::AttachGlob(&layout.initial_params[i]);
      }
    }
  }
  return layout;
}

ParamBindingLayout DescribeParams(const BatchState& state) {
  return DescribeParams(*state.plan);
}

ParamBindingLayout DescribeParams(const RowState& state) {
  return DescribeParams(*state.plan);
}

}  // namespace internal

namespace {

std::vector<uint32_t> OutputColumnIndices(const PhysicalPlan& plan) {
  const auto& cols = plan.columns();
  std::vector<uint32_t> indices;
  indices.reserve(cols.size());
  for (const PhysicalPlan::Column& col : cols) {
    indices.push_back(col.index);
  }
  return indices;
}

}  // namespace

// -----------------------------------------------------------------------------
// BatchIterator
// -----------------------------------------------------------------------------

BatchIterator::BatchIterator(std::unique_ptr<PhysicalPlan> plan)
    : owned_(std::make_unique<internal::BatchState>(std::move(plan))),
      source_(&owned_->plan->source()),
      context_(&owned_->context),
      pool_(owned_->plan->pool()),
      column_indices_(OutputColumnIndices(*owned_->plan)) {
  owned_->context.SetParams(owned_->plan->params());
}

BatchIterator::BatchIterator(BatchIterator&&) noexcept = default;
BatchIterator& BatchIterator::operator=(BatchIterator&&) noexcept = default;
BatchIterator::~BatchIterator() = default;

void BatchIterator::Open() {
  Open(BoundParams(owned_->plan->params()));
}

void BatchIterator::Open(BoundParams params) {
  owned_->context.ClearError();
  owned_->context.SetParams(params.params_);
  core::exec::Restart(*source_, *owned_->op_state);
  exhausted_ = false;
}

std::optional<Batch> BatchIterator::Next() {
  if (PERFETTO_UNLIKELY(exhausted_)) {
    return std::nullopt;
  }
  for (;;) {
    core::exec::RowBatch* batch =
        source_->Next(owned_->scratch, *owned_->op_state);
    if (!batch) {
      return std::nullopt;
    }
    exhausted_ = batch->last();
    if (batch->size() > 0) {
      return Batch(batch, column_indices_.data(),
                   static_cast<uint32_t>(column_indices_.size()), pool_);
    }
    if (exhausted_) {
      return std::nullopt;
    }
  }
}

const std::string& BatchIterator::column_name(uint32_t output_col) const {
  return owned_->plan->columns()[output_col].name;
}

// -----------------------------------------------------------------------------
// RowIterator
// -----------------------------------------------------------------------------

RowIterator::RowIterator(std::unique_ptr<PhysicalPlan> plan)
    : owned_(std::make_unique<internal::RowState>(std::move(plan))),
      cursor_(&owned_->cursor),
      pool_(owned_->plan->pool()),
      column_indices_(OutputColumnIndices(*owned_->plan)) {}

RowIterator::RowIterator(RowIterator&&) noexcept = default;
RowIterator& RowIterator::operator=(RowIterator&&) noexcept = default;
RowIterator::~RowIterator() = default;

bool RowIterator::Open() {
  return cursor_->Open(owned_->plan->params());
}

const std::string& RowIterator::column_name(uint32_t output_col) const {
  return owned_->plan->columns()[output_col].name;
}

}  // namespace perfetto::trace_processor::pipeline
