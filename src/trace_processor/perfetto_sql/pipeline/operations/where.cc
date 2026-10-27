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
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

// Hands a WHERE predicate's literals to the shared filter value casts, so they
// mean for each column type what any other filter value means. A scalar is the
// first literal; IN iterates them all.
struct LiteralFetcher : core::ValueFetcher {
  static constexpr Type kInt64 = 0;
  static constexpr Type kDouble = 1;
  static constexpr Type kString = 2;
  static constexpr Type kNull = 3;

  LiteralFetcher(const WhereLiteral* l,
                 uint32_t n,
                 const std::vector<std::string>& s)
      : literals(l), count(n), strings(s) {}

  Type GetValueType(uint32_t) const {
    switch (literals[at].type) {
      case WhereLiteral::Type::kInt64:
        return kInt64;
      case WhereLiteral::Type::kDouble:
        return kDouble;
      case WhereLiteral::Type::kString:
        return kString;
    }
    PERFETTO_FATAL("Unknown literal type");
  }
  int64_t GetInt64Value(uint32_t) const { return literals[at].int_value; }
  double GetDoubleValue(uint32_t) const { return literals[at].double_value; }
  const char* GetStringValue(uint32_t) const {
    return strings[static_cast<size_t>(literals[at].int_value)].c_str();
  }
  bool IteratorInit(uint32_t) {
    at = 0;
    return count != 0;
  }
  bool IteratorNext(uint32_t) { return ++at < count; }

  const WhereLiteral* literals;
  uint32_t count;
  const std::vector<std::string>& strings;
  uint32_t at = 0;
};

// Parses a WHERE condition into the predicates, literals and strings of a
// Where.
class Parser {
 public:
  Parser(Compiler* c,
         std::vector<WherePredicate>* predicates,
         std::vector<WhereLiteral>* literals,
         std::vector<std::string>* strings)
      : c_(c),
        predicates_(predicates),
        literals_(literals),
        strings_(strings) {}

  base::Status Condition(uint32_t expr) {
    const auto* node = Node<SyntaqliteNode>(c_->parser(), expr);
    if (node->tag == SYNTAQLITE_NODE_BINARY_EXPR &&
        node->binary_expr.op == SYNTAQLITE_BINARY_OP_AND) {
      RETURN_IF_ERROR(Condition(node->binary_expr.left));
      return Condition(node->binary_expr.right);
    }
    switch (static_cast<int>(node->tag)) {
      case SYNTAQLITE_NODE_BINARY_EXPR:
        return Comparison(expr, node->binary_expr);
      case SYNTAQLITE_NODE_LIKE_EXPR:
        return Like(expr, node->like_expr);
      case SYNTAQLITE_NODE_IN_EXPR:
        return In(expr, node->in_expr);
      case SYNTAQLITE_NODE_IS_EXPR:
        return Is(expr, node->is_expr);
      case SYNTAQLITE_NODE_BETWEEN_EXPR:
        return Between(expr, node->between_expr);
      default:
        return c_->Unsupported(expr, "a condition other than column OP value");
    }
  }

 private:
  base::Status Comparison(uint32_t expr, const SyntaqliteBinaryExpr& node) {
    std::optional<core::Op> op = ComparisonOp(node.op);
    if (!op) {
      return c_->Unsupported(expr, kOperators);
    }
    ASSIGN_OR_RETURN(ColumnId column, c_->ResolveColumnExpr(node.left));
    return Add(column, *op, {node.right});
  }

  base::Status Like(uint32_t expr, const SyntaqliteLikeExpr& node) {
    if (node.keyword != SYNTAQLITE_LIKE_KEYWORD_GLOB) {
      return c_->Unsupported(expr, kOperators);
    }
    if (node.negated) {
      return c_->Unsupported(expr, "NOT GLOB");
    }
    ASSIGN_OR_RETURN(ColumnId column, c_->ResolveColumnExpr(node.operand));
    return Add(column, core::Glob{}, {node.pattern});
  }

  base::Status In(uint32_t expr, const SyntaqliteInExpr& node) {
    if (node.negated) {
      return c_->Unsupported(expr, "NOT IN");
    }
    const auto* source = Node<SyntaqliteNode>(c_->parser(), node.source);
    if (node.bare_source || source->tag != SYNTAQLITE_NODE_EXPR_LIST) {
      return c_->Unsupported(expr, "IN other than a list of literals");
    }
    ASSIGN_OR_RETURN(ColumnId column, c_->ResolveColumnExpr(node.operand));
    const SyntaqliteExprList& list = source->expr_list;
    return Add(
        column, core::In{},
        std::vector<uint32_t>(list.children, list.children + list.count));
  }

  base::Status Is(uint32_t expr, const SyntaqliteIsExpr& node) {
    bool null;
    switch (static_cast<int>(node.op)) {
      case SYNTAQLITE_IS_OP_IS_NULL:
        null = true;
        break;
      case SYNTAQLITE_IS_OP_NOT_NULL:
      case SYNTAQLITE_IS_OP_NOT_NULL_SPACED:
        null = false;
        break;
      case SYNTAQLITE_IS_OP_IS:
      case SYNTAQLITE_IS_OP_IS_NOT: {
        const auto* right = Node<SyntaqliteNode>(c_->parser(), node.right);
        if (right->tag != SYNTAQLITE_NODE_LITERAL ||
            right->literal.literal_type != SYNTAQLITE_LITERAL_TYPE_NULL) {
          return c_->Unsupported(expr, "IS other than IS [NOT] NULL");
        }
        null = node.op == SYNTAQLITE_IS_OP_IS;
        break;
      }
      default:
        return c_->Unsupported(expr, "IS other than IS [NOT] NULL");
    }
    ASSIGN_OR_RETURN(ColumnId column, c_->ResolveColumnExpr(node.left));
    core::Op op = null ? core::Op(core::IsNull{}) : core::Op(core::IsNotNull{});
    return Add(column, op, {});
  }

  base::Status Between(uint32_t expr, const SyntaqliteBetweenExpr& node) {
    if (node.negated) {
      return c_->Unsupported(expr, "NOT BETWEEN");
    }
    ASSIGN_OR_RETURN(ColumnId column, c_->ResolveColumnExpr(node.operand));
    RETURN_IF_ERROR(Add(column, core::Ge{}, {node.low}));
    return Add(column, core::Le{}, {node.high});
  }

  base::Status Add(ColumnId column,
                   core::Op op,
                   const std::vector<uint32_t>& exprs) {
    WherePredicate predicate;
    predicate.column = column;
    predicate.op = op;
    predicate.first_literal = static_cast<uint32_t>(literals_->size());
    predicate.literal_count = static_cast<uint32_t>(exprs.size());
    for (uint32_t expr : exprs) {
      RETURN_IF_ERROR(Literal(expr));
    }
    predicates_->push_back(predicate);
    return base::OkStatus();
  }

  // Reads a number literal, optionally negated, or a string literal.
  base::Status Literal(uint32_t expr) {
    SyntaqliteParser* p = c_->parser();
    const auto* node = Node<SyntaqliteNode>(p, expr);
    bool negate = false;
    if (node->tag == SYNTAQLITE_NODE_UNARY_EXPR &&
        node->unary_expr.op == SYNTAQLITE_UNARY_OP_MINUS) {
      negate = true;
      node = Node<SyntaqliteNode>(p, node->unary_expr.operand);
    }
    if (node->tag != SYNTAQLITE_NODE_LITERAL) {
      return c_->Unsupported(expr, "a value other than a literal");
    }
    const SyntaqliteLiteral& literal = node->literal;
    std::string text = SpanText(p, literal.source);
    WhereLiteral out;
    switch (static_cast<int>(literal.literal_type)) {
      case SYNTAQLITE_LITERAL_TYPE_INTEGER: {
        std::optional<int64_t> value = base::StringToInt64(text);
        if (!value) {
          return c_->Unsupported(expr,
                                 "a number which does not fit an integer");
        }
        out.type = WhereLiteral::Type::kInt64;
        out.int_value = negate ? -*value : *value;
        break;
      }
      case SYNTAQLITE_LITERAL_TYPE_FLOAT: {
        std::optional<double> value = base::StringToDouble(text);
        if (!value) {
          return c_->Unsupported(expr, "a malformed number");
        }
        out.type = WhereLiteral::Type::kDouble;
        out.double_value = negate ? -*value : *value;
        break;
      }
      case SYNTAQLITE_LITERAL_TYPE_STRING:
        if (negate) {
          return c_->Unsupported(expr, "a negated string");
        }
        out.type = WhereLiteral::Type::kString;
        out.int_value = static_cast<int64_t>(strings_->size());
        strings_->push_back(StringLiteralText(text));
        break;
      default:
        return c_->Unsupported(expr, "a literal other than a number or string");
    }
    literals_->push_back(out);
    return base::OkStatus();
  }

  static std::optional<core::Op> ComparisonOp(SyntaqliteBinaryOp op) {
    switch (static_cast<int>(op)) {
      case SYNTAQLITE_BINARY_OP_EQ:
      case SYNTAQLITE_BINARY_OP_EQ_DOUBLE:
        return core::Eq{};
      case SYNTAQLITE_BINARY_OP_NE:
      case SYNTAQLITE_BINARY_OP_NE_ANGLE:
        return core::Ne{};
      case SYNTAQLITE_BINARY_OP_LT:
        return core::Lt{};
      case SYNTAQLITE_BINARY_OP_LE:
        return core::Le{};
      case SYNTAQLITE_BINARY_OP_GT:
        return core::Gt{};
      case SYNTAQLITE_BINARY_OP_GE:
        return core::Ge{};
      default:
        return std::nullopt;
    }
  }

  // The text of 'it''s' is it's: the span keeps the quotes and doubles them.
  static std::string StringLiteralText(const std::string& source) {
    PERFETTO_CHECK(source.size() >= 2 && source.front() == '\'' &&
                   source.back() == '\'');
    return base::ReplaceAll(source.substr(1, source.size() - 2), "''", "'");
  }

  static constexpr char kOperators[] =
      "an operator other than =, !=, <, <=, >, >=, GLOB, IN, BETWEEN and "
      "IS [NOT] NULL";

  Compiler* c_;
  std::vector<WherePredicate>* predicates_;
  std::vector<WhereLiteral>* literals_;
  std::vector<std::string>* strings_;
};

// The type a column of no known type is converted to, for Filter to read it:
// the type its literals have. Strings win, as SQL compares a number with a
// string by type. None for IS [NOT] NULL, which reads any column.
std::optional<core::StorageType> TypeForLiterals(
    const WherePredicate& p,
    const std::vector<WhereLiteral>& literals) {
  if (p.op.IsAnyOf<core::NullOp>()) {
    return std::nullopt;
  }
  if (p.op.Is<core::Glob>()) {
    return core::String{};
  }
  core::StorageType type = core::Int64{};
  for (uint32_t i = 0; i < p.literal_count; ++i) {
    switch (literals[p.first_literal + i].type) {
      case WhereLiteral::Type::kString:
        return core::String{};
      case WhereLiteral::Type::kDouble:
        type = core::Double{};
        break;
      case WhereLiteral::Type::kInt64:
        break;
    }
  }
  return type;
}

}  // namespace

const OperationRegistration Where::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_WHERE, &BuildPlan,
    OperationRegistration::Encoding{8, &MakeOperation<Where>}};

base::Status Where::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("WHERE");
  const auto* n = Node<SyntaqlitePerfettoPipeWhere>(c->parser(), stage);
  Where where;
  Parser parser(c, &where.predicates_, &where.literals_, &where.strings_);
  RETURN_IF_ERROR(parser.Condition(n->condition));
  c->AddNode(std::move(where), {c->plan().root()});
  return base::OkStatus();
}

std::optional<uint32_t> Where::Prune(std::vector<bool>* needed) {
  for (const WherePredicate& p : predicates_) {
    (*needed)[p.column] = true;
  }
  return std::nullopt;
}

void Where::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerNode(node.children()[0]);
  std::vector<ex::Filter::Condition> conditions;
  for (const WherePredicate& p : predicates_) {
    ex::Filter::Condition condition;
    condition.column = c->QueryPosition(p.column);
    condition.op = p.op;
    // Filter reads only columns of one type, but for IS [NOT] NULL. A column
    // of no known type carries a type per row, so it is converted to the
    // literals' type. GLOB matches only strings.
    std::optional<core::StorageType> type = c->QueryType(p.column).storage();
    std::optional<core::StorageType> wanted = TypeForLiterals(p, literals_);
    if (wanted && (!type || p.op.Is<core::Glob>())) {
      type = wanted;
      c->RequireType(p.column, *type);
    }
    if (type && !p.op.IsAnyOf<core::NullOp>()) {
      LiteralFetcher fetcher(literals_.data() + p.first_literal,
                             p.literal_count, strings_);
      ex::FilterValue value;
      if (p.op.Is<core::In>()) {
        ex::GetFilterValueListCaster<LiteralFetcher> (*type)(
            fetcher, 0, *c->context().pool, &value);
      } else {
        ex::GetFilterValueCaster<LiteralFetcher>(*type, p.op)(
            fetcher, 0, *c->context().pool, &value);
      }
      condition.value = c->AddFilterValue(std::move(value));
    }
    conditions.push_back(condition);
  }
  c->AddOperator(
      std::make_unique<ex::Filter>(std::move(conditions), c->context().pool));
  c->DeclareSmallBatches();
  c->DeclareRowsDropped();
}

const OperationRegistration& Where::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> Where::Clone() const {
  return std::make_unique<Where>(*this);
}

void Where::Serialize(PlanCodec& c) {
  c.Vec(&predicates_);
  c.Vec(&literals_);
  strings_.resize(c.Count(strings_.size()));
  for (std::string& text : strings_) {
    c.Str(&text);
  }
}

}  // namespace perfetto::trace_processor::pipeline
