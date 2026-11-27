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

#include "src/trace_processor/perfetto_sql/pipeline/operations/where.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

// The value `p` compares with, if whoever runs the plan gives it: an external
// literal or a bound param.
const Literal* GivenOf(const WherePredicate& p,
                       base::Span<const Literal> literals) {
  if (p.literal_count != 1 || !literals[p.first_literal].is_given()) {
    return nullptr;
  }
  return &literals[p.first_literal];
}

std::optional<std::string> LiteralMismatch(const WherePredicate& p,
                                           base::Span<const Literal> literals,
                                           core::StorageType type) {
  return CheckLiteralTypes(
      p.op,
      base::Span<const Literal>(
          literals.data() + p.first_literal,
          literals.data() + p.first_literal + p.literal_count),
      type);
}

// The type a column of no known type is converted to, for Filter to read it:
// the type its literals have. Strings win, as SQL compares a number with a
// string by type. None for IS [NOT] NULL, which reads any column, or for a
// given value, which has none yet. GLOB and REGEXP match only strings,
// whatever is given.
std::optional<core::StorageType> TypeForLiterals(
    const WherePredicate& p,
    base::Span<const Literal> literals) {
  if (p.op.IsAnyOf<core::NullOp>()) {
    return std::nullopt;
  }
  if (p.op.IsAnyOf<core::OnlyStringOp>()) {
    return core::String{};
  }
  if (GivenOf(p, literals)) {
    return std::nullopt;
  }
  core::StorageType type = core::Int64{};
  for (uint32_t i = 0; i < p.literal_count; ++i) {
    const Literal& literal = literals[p.first_literal + i];
    if (literal.is_string()) {
      return core::String{};
    }
    if (literal.is_double()) {
      type = core::Double{};
    }
  }
  return type;
}

// The value `p` tests a column of storage `type` with: its literals, cast now
// unless they are a bound param.
PlannedParam PlanValue(const WherePredicate& p,
                       base::Span<const Literal> literals,
                       core::StorageType type,
                       const StringPool& pool) {
  PlannedParam value;
  value.literals.assign(literals.begin() + p.first_literal,
                        literals.begin() + p.first_literal + p.literal_count);
  value.type = type;
  value.op = p.op;
  if (const Literal* given = GivenOf(p, literals)) {
    value.state = given->is_bound() ? PlannedParam::State::kBound
                                    : PlannedParam::State::kExternal;
  } else {
    value.state = PlannedParam::State::kConstant;
    value.value =
        CastLiterals(p.op, value.literals.data(), p.literal_count, type, pool);
  }
  return value;
}

}  // namespace

constexpr OperationRegistration Where::kRegistration{8, &MakeOperation<Where>};

void Where::AddToPlan(LogicalPlan* plan,
                      const std::vector<WherePredicate>& predicates,
                      const std::vector<Literal>& literals) {
  Where where;
  where.predicates_ = plan->arena().CopySpan<WherePredicate>(predicates);
  where.literals_ = plan->arena().CopySpan<Literal>(literals);
  plan->AddNode(where, {plan->root()});
}

std::optional<uint32_t> Where::Prune(std::vector<bool>* needed) {
  for (const WherePredicate& p : predicates_) {
    (*needed)[p.column] = true;
  }
  return std::nullopt;
}

InputNeeds Where::Needs(uint32_t) const {
  // Filter reads only columns of one type, but for IS [NOT] NULL. A column of
  // no known type carries a type per row, so it is converted to the literals'
  // type. GLOB and REGEXP match only strings.
  InputNeeds needs;
  for (const WherePredicate& p : predicates_) {
    if (std::optional<core::StorageType> wanted =
            TypeForLiterals(p, literals_.span())) {
      needs.types_first.push_back(
          {p.column, *wanted, !p.op.IsAnyOf<core::OnlyStringOp>()});
    }
  }
  return needs;
}

OutputProperties Where::Derive(DeriveContext& c) const {
  OutputProperties out = c.input(0);
  out.DropRows();
  std::vector<PlannedParam>& values = c.params();
  values.resize(predicates_.size());
  for (uint32_t i = 0; i < predicates_.size(); ++i) {
    const WherePredicate& p = predicates_[i];
    // A rewrite may have planned it already.
    if (values[i].state != PlannedParam::State::kUnplanned) {
      continue;
    }
    if (p.op.IsAnyOf<core::NullOp>()) {
      values[i].state = PlannedParam::State::kNone;
      continue;
    }
    std::optional<core::StorageType> type = out.TypeOf(p.column).storage();
    if (!type) {
      if (!GivenOf(p, literals_.span())) {
        continue;
      }
      // A column holding no values matches no comparison, whatever is given.
      if (!out.TypeOf(p.column).is_null()) {
        c.Fail(
            base::ErrStatus("WHERE: a given value needs a column of a known "
                            "type"));
        return out;
      }
      values[i].state = PlannedParam::State::kConstant;
      continue;
    }
    if (std::optional<std::string> mismatch =
            LiteralMismatch(p, literals_.span(), *type)) {
      c.Fail(base::ErrStatus("WHERE: expected %s", mismatch->c_str()));
      return out;
    }
    values[i] = PlanValue(p, literals_.span(), *type, *c.context().pool);
  }
  return out;
}

void Where::Rewrite(BoundPlan* bound,
                    PlanNodeId self,
                    const LoweringContext& context) const {
  LogicalPlan& plan = bound->plan;
  PlanNode& node = plan.nodes()[self];
  PlanNodeId scan_id = node.children()[0];
  PlanNode& below = plan.nodes()[scan_id];
  if (!below.Is<Scan>() || below.Cast<Scan>().has_search()) {
    return;
  }
  auto& where = node.Cast<Where>();
  auto& scan = below.Cast<Scan>();

  // A scan converts no column, so only conditions on columns of a known type,
  // read as the table stores them, have values to search with. They are what
  // these conditions test whether or not the scan answers them.
  std::vector<PlannedParam> values(where.predicates_.size());
  std::vector<uint32_t> predicate_indices;
  std::vector<Scan::Condition> conditions;
  for (uint32_t i = 0; i < where.predicates_.size(); ++i) {
    const WherePredicate& p = where.predicates_[i];
    std::optional<core::StorageType> type =
        bound->planned[scan_id].output.TypeOf(p.column).storage();
    if (!type || p.op.IsAnyOf<core::NullOp>() ||
        (p.op.IsAnyOf<core::OnlyStringOp>() && !type->Is<core::String>()) ||
        LiteralMismatch(p, where.literals_.span(), *type)) {
      continue;
    }
    values[i] = PlanValue(p, where.literals_.span(), *type, *context.pool);
    predicate_indices.push_back(i);
    conditions.push_back({p.column, p.op,
                          values[i].state == PlannedParam::State::kConstant
                              ? &values[i].value
                              : nullptr});
  }
  std::optional<Scan::SearchPlan> searched =
      scan.PlanSearch(conditions, *bound->bindings.dataframes[scan_id]);
  if (!searched) {
    bound->planned[self].params = std::move(values);
    return;
  }

  // The answered conditions move into the scan with their values.
  ScanSearch search;
  search.kind = searched->kind;
  search.index = searched->index;
  search.sorted = searched->sorted;
  search.predicates = plan.arena().NewSpan<WherePredicate>(
      static_cast<uint32_t>(searched->answered.size()));
  std::vector<PlannedParam>& scan_values = bound->planned[scan_id].params;
  std::vector<bool> moved(where.predicates_.size(), false);
  for (uint32_t k = 0; k < searched->answered.size(); ++k) {
    uint32_t i = predicate_indices[searched->answered[k]];
    moved[i] = true;
    search.predicates[k] = where.predicates_[i];
    scan_values.push_back(std::move(values[i]));
  }
  scan.SetSearch(search);
  PlanNodeInputs(bound, scan_id, context);
  uint32_t kept = 0;
  std::vector<PlannedParam> kept_values;
  for (uint32_t i = 0; i < where.predicates_.size(); ++i) {
    if (!moved[i]) {
      where.predicates_[kept++] = where.predicates_[i];
      kept_values.push_back(std::move(values[i]));
    }
  }
  where.predicates_.Shrink(kept);
  bound->planned[self].params = std::move(kept_values);
  if (where.predicates_.empty()) {
    plan.Bypass(self);
  }
}

void Where::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerInput(node, 0);
  auto conditions = c->arena().NewSpan<ex::Filter::Condition>(
      static_cast<uint32_t>(predicates_.size()));
  for (uint32_t i = 0; i < predicates_.size(); ++i) {
    ex::Filter::Condition condition;
    condition.column = c->QueryPosition(predicates_[i].column);
    condition.op = predicates_[i].op;
    if (std::optional<uint32_t> value = c->TakeParam(node, i)) {
      condition.param = *value;
    }
    conditions[i] = condition;
  }
  c->AddOperator(c->arena().New<ex::Filter>(conditions, c->context().pool));
}

const OperationRegistration& Where::registration() const {
  return kRegistration;
}

void Where::Serialize(PlanCodec& c) {
  c.Vec(&predicates_);
  c.Resize(&literals_);
  for (Literal& literal : literals_) {
    SerializeLiteral(c, &literal);
  }
}

}  // namespace perfetto::trace_processor::pipeline
