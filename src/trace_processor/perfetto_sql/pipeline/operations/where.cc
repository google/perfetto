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
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/cost_estimation.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

// Hands a WHERE literal to the shared filter value casts, so it means for
// each column type what any other filter value means.
struct LiteralFetcher : core::ValueFetcher {
  static constexpr Type kInt64 = 0;
  static constexpr Type kDouble = 1;
  static constexpr Type kString = 2;
  static constexpr Type kNull = 3;

  explicit LiteralFetcher(const WherePredicate& p) : predicate(p) {}

  Type GetValueType(uint32_t) const {
    return predicate.is_double ? kDouble : kInt64;
  }
  int64_t GetInt64Value(uint32_t) const { return predicate.int_value; }
  double GetDoubleValue(uint32_t) const { return predicate.double_value; }
  const char* GetStringValue(uint32_t) const {
    PERFETTO_FATAL("WHERE literals are numbers");
  }

  const WherePredicate& predicate;
};

// Reads a number literal, optionally negated, into `out`.
base::Status ParseLiteral(Compiler* c, uint32_t expr, WherePredicate* out) {
  SyntaqliteParser* p = c->parser();
  const auto* node = Node<SyntaqliteNode>(p, expr);
  bool negate = false;
  if (node->tag == SYNTAQLITE_NODE_UNARY_EXPR &&
      node->unary_expr.op == SYNTAQLITE_UNARY_OP_MINUS) {
    negate = true;
    node = Node<SyntaqliteNode>(p, node->unary_expr.operand);
  }
  if (node->tag != SYNTAQLITE_NODE_LITERAL) {
    return c->Unsupported(expr, "a value other than a number literal");
  }
  const SyntaqliteLiteral& literal = node->literal;
  std::string text = SpanText(p, literal.source);
  if (literal.literal_type == SYNTAQLITE_LITERAL_TYPE_INTEGER) {
    std::optional<int64_t> value = base::StringToInt64(text);
    if (!value) {
      return c->Unsupported(expr, "a number which does not fit an integer");
    }
    out->is_double = false;
    out->int_value = negate ? -*value : *value;
    return base::OkStatus();
  }
  if (literal.literal_type == SYNTAQLITE_LITERAL_TYPE_FLOAT) {
    std::optional<double> value = base::StringToDouble(text);
    if (!value) {
      return c->Unsupported(expr, "a malformed number");
    }
    out->is_double = true;
    out->double_value = negate ? -*value : *value;
    return base::OkStatus();
  }
  return c->Unsupported(expr, "a literal other than a number");
}

base::StatusOr<WherePredicate> ParsePredicate(Compiler* c, uint32_t expr) {
  const auto* node = Node<SyntaqliteNode>(c->parser(), expr);
  if (node->tag != SYNTAQLITE_NODE_BINARY_EXPR) {
    return c->Unsupported(expr, "a condition other than column OP value");
  }
  WherePredicate predicate;
  SyntaqliteBinaryOp op = node->binary_expr.op;
  if (op == SYNTAQLITE_BINARY_OP_EQ) {
    predicate.op = core::Eq{};
  } else if (op == SYNTAQLITE_BINARY_OP_NE) {
    predicate.op = core::Ne{};
  } else if (op == SYNTAQLITE_BINARY_OP_LT) {
    predicate.op = core::Lt{};
  } else if (op == SYNTAQLITE_BINARY_OP_LE) {
    predicate.op = core::Le{};
  } else if (op == SYNTAQLITE_BINARY_OP_GT) {
    predicate.op = core::Gt{};
  } else if (op == SYNTAQLITE_BINARY_OP_GE) {
    predicate.op = core::Ge{};
  } else {
    return c->Unsupported(expr, "an operator other than =, !=, <, <=, >, >=");
  }
  ASSIGN_OR_RETURN(predicate.column,
                   c->ResolveColumnExpr(node->binary_expr.left));
  RETURN_IF_ERROR(ParseLiteral(c, node->binary_expr.right, &predicate));
  return predicate;
}

base::Status ParseCondition(Compiler* c,
                            uint32_t expr,
                            std::vector<WherePredicate>* out) {
  const auto* node = Node<SyntaqliteNode>(c->parser(), expr);
  if (node->tag == SYNTAQLITE_NODE_BINARY_EXPR &&
      node->binary_expr.op == SYNTAQLITE_BINARY_OP_AND) {
    RETURN_IF_ERROR(ParseCondition(c, node->binary_expr.left, out));
    return ParseCondition(c, node->binary_expr.right, out);
  }
  ASSIGN_OR_RETURN(WherePredicate predicate, ParsePredicate(c, expr));
  out->push_back(predicate);
  return base::OkStatus();
}

}  // namespace

const OperationRegistration Where::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_WHERE, &BuildPlan,
    OperationRegistration::Encoding{8, false, &DecodePlan}};

base::Status Where::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("WHERE");
  const auto* n = Node<SyntaqlitePerfettoPipeWhere>(c->parser(), stage);
  Where where;
  RETURN_IF_ERROR(ParseCondition(c, n->condition, &where.predicates_));
  c->AddNode(std::move(where), {c->plan().root()});
  return base::OkStatus();
}

std::optional<uint32_t> Where::Prune(std::vector<bool>* needed) {
  for (const WherePredicate& p : predicates_) {
    (*needed)[p.column] = true;
  }
  return std::nullopt;
}

core::RowEstimate Where::Estimate(PlanEstimator* e,
                                  const PlanNode& node) const {
  // Estimated as it is lowered: straight on a scan, from what the scan's
  // columns say of their values.
  const PlanNode& child = e->plan().nodes()[node.children()[0]];
  if (child.Is<Scan>()) {
    return child.Cast<Scan>().EstimateFiltered(e, predicates_);
  }
  return e->ApplyBlind(e->EstimateNode(node.children()[0]), predicates_);
}

uint32_t Where::LowerValue(Lowering* c,
                           const WherePredicate& p,
                           core::StorageType type) {
  if (p.param) {
    // Cast as each run starts, from what the plan is passed.
    return c->AddFilterParam(*p.param, type, p.op);
  }
  if (p.op.IsAnyOf<core::NullOp>()) {
    return 0;
  }
  LiteralFetcher fetcher(p);
  ex::FilterValue value;
  ex::GetFilterValueCaster<LiteralFetcher>(type, p.op)(
      fetcher, 0, *c->context().pool, &value);
  return c->AddFilterValue(std::move(value));
}

void Where::Lower(Lowering* c, const PlanNode& node) const {
  // Straight on a table, the predicates its sorted columns or an index can
  // find the rows of are searched for, and only the rest tested.
  const PlanNode& child = c->plan().nodes()[node.children()[0]];
  std::optional<std::vector<bool>> searched;
  if (child.Is<Scan>()) {
    searched = child.Cast<Scan>().LowerSearching(c, predicates_);
  }
  if (!searched) {
    c->LowerNode(node.children()[0]);
    searched = std::vector<bool>(predicates_.size());
  }
  std::vector<ex::Filter::Condition> conditions;
  for (uint32_t i = 0; i < predicates_.size(); ++i) {
    if ((*searched)[i]) {
      continue;
    }
    const WherePredicate& p = predicates_[i];
    // Filter reads only columns of one type, and casts its values for the
    // type the executor holds the column as. A column which carries a type
    // per row is converted: to the literal's, or to the type the plan says
    // it holds.
    ex::ColumnLayout layout = c->QueryLayout(p.column);
    core::StorageType type = layout.type();
    if (layout.variant()) {
      std::optional<core::StorageType> known = c->QueryType(p.column).storage();
      if (!p.param && !p.op.IsAnyOf<core::NullOp>()) {
        type = p.is_double ? core::StorageType(core::Double{})
                           : core::StorageType(core::Int64{});
      } else if (known) {
        type = *known;
      } else {
        c->Fail(base::ErrStatus("WHERE: column '%s' has no type to compare by",
                                c->plan().columns()[p.column].name.c_str()));
        return;
      }
      c->RequireType(p.column, type);
    }

    ex::Filter::Condition condition;
    condition.column = c->QueryPosition(p.column);
    condition.op = p.op;
    condition.value = LowerValue(c, p, type);
    conditions.push_back(condition);
  }
  if (conditions.empty()) {
    return;
  }
  c->AddOperator(
      std::make_unique<ex::Filter>(std::move(conditions), c->context().pool));
  c->DeclareSmallBatches();
}

const OperationRegistration& Where::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> Where::Clone() const {
  return std::make_unique<Where>(*this);
}

void Where::Write(PlanWriter* c, const PlanNode&, Available* available) const {
  WritePredicates(c, predicates_, *available);
}

void Where::DecodePlan(PlanReader* c, Available* available) {
  Where where(ReadPredicates(c, *available));
  if (where.predicates_.empty()) {
    c->reader().Fail();
  }
  c->AddNode(std::move(where), {c->plan().root()});
}

void Where::AddToPlan(LogicalPlan* plan,
                      std::vector<WherePredicate> predicates) {
  PlanNode& root = plan->nodes()[plan->root()];
  if (root.Is<Where>()) {
    auto& where = static_cast<Where&>(root.operation());
    where.predicates_.insert(where.predicates_.end(), predicates.begin(),
                             predicates.end());
    return;
  }
  plan->AddNode(Where(std::move(predicates)), {plan->root()});
}

namespace {

// What a predicate compares with, as written.
enum ValueKind : uint8_t { kInt = 0, kDouble = 1, kParam = 2, kNone = 3 };

}  // namespace

void Where::WritePredicates(PlanWriter* c,
                            const std::vector<WherePredicate>& predicates,
                            const Available& available) {
  c->writer().Size(predicates.size());
  for (const WherePredicate& p : predicates) {
    c->writer().Position(available, p.column);
    c->writer().U8(static_cast<uint8_t>(p.op.index()));
    if (p.op.IsAnyOf<core::NullOp>()) {
      c->writer().U8(kNone);
    } else if (p.param) {
      c->writer().U8(kParam);
      c->writer().U32(*p.param);
    } else if (p.is_double) {
      c->writer().U8(kDouble);
      c->writer().F64(p.double_value);
    } else {
      c->writer().U8(kInt);
      c->writer().U64(static_cast<uint64_t>(p.int_value));
    }
  }
}

std::vector<WherePredicate> Where::ReadPredicates(PlanReader* c,
                                                  const Available& available) {
  std::vector<WherePredicate> predicates(c->reader().Count());
  for (WherePredicate& p : predicates) {
    p.column = c->reader().Position(available);
    // Only comparisons, null tests and IN are written, each with the value it
    // compares with; anything else is corrupt.
    std::optional<core::Op> op = core::Op::FromIndex(c->reader().U8());
    uint8_t kind = c->reader().U8();
    bool comparison = op && op->IsAnyOf<core::NonStringOp>() &&
                      (kind == kInt || kind == kDouble || kind == kParam);
    bool null_test = op && op->IsAnyOf<core::NullOp>() && kind == kNone;
    bool in = op && op->Is<core::In>() && kind == kParam;
    if (!comparison && !null_test && !in) {
      c->reader().Fail();
      continue;
    }
    p.op = *op;
    if (kind == kParam) {
      p.param = c->reader().U32();
    } else if (kind == kDouble) {
      p.is_double = true;
      p.double_value = c->reader().F64();
    } else if (kind == kInt) {
      p.int_value = static_cast<int64_t>(c->reader().U64());
    }
  }
  return predicates;
}

}  // namespace perfetto::trace_processor::pipeline
