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

#include "src/trace_processor/perfetto_sql/pipeline/literal.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

// Hands a condition's literals to the shared filter value casts, so they
// mean for each column type what any other filter value means. A scalar is the
// first literal; IN iterates them all.
struct LiteralFetcher : core::ValueFetcher {
  static constexpr Type kInt64 = 0;
  static constexpr Type kDouble = 1;
  static constexpr Type kString = 2;
  static constexpr Type kNull = 3;

  LiteralFetcher(const ConditionLiteral* l,
                 uint32_t n,
                 const std::vector<std::string>& s)
      : literals(l), count(n), strings(s) {}

  Type GetValueType(uint32_t) const {
    switch (literals[at].type) {
      case ConditionLiteral::Type::kInt64:
        return kInt64;
      case ConditionLiteral::Type::kDouble:
        return kDouble;
      case ConditionLiteral::Type::kString:
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

  const ConditionLiteral* literals;
  uint32_t count;
  const std::vector<std::string>& strings;
  uint32_t at = 0;
};

// The text of 'it''s' is it's: the span keeps the quotes and doubles them.
std::string StringLiteralText(const std::string& source) {
  PERFETTO_CHECK(source.size() >= 2 && source.front() == '\'' &&
                 source.back() == '\'');
  return base::ReplaceAll(source.substr(1, source.size() - 2), "''", "'");
}

}  // namespace

base::Status CompileConditionLiteral(Compiler* c,
                                     uint32_t expr,
                                     std::vector<ConditionLiteral>* literals,
                                     std::vector<std::string>* strings) {
  SyntaqliteParser* p = c->parser();
  const auto* node = Node<SyntaqliteNode>(p, expr);
  bool negate = false;
  if (node->tag == SYNTAQLITE_NODE_UNARY_EXPR &&
      node->unary_expr.op == SYNTAQLITE_UNARY_OP_MINUS) {
    negate = true;
    node = Node<SyntaqliteNode>(p, node->unary_expr.operand);
  }
  if (node->tag != SYNTAQLITE_NODE_LITERAL) {
    return c->Unsupported(expr, "a value other than a literal");
  }
  const SyntaqliteLiteral& literal = node->literal;
  std::string text = SpanText(p, literal.source);
  ConditionLiteral out;
  switch (static_cast<int>(literal.literal_type)) {
    case SYNTAQLITE_LITERAL_TYPE_INTEGER: {
      std::optional<int64_t> value = base::StringToInt64(text);
      if (!value) {
        return c->Unsupported(expr, "a number which does not fit an integer");
      }
      out.type = ConditionLiteral::Type::kInt64;
      out.int_value = negate ? -*value : *value;
      break;
    }
    case SYNTAQLITE_LITERAL_TYPE_FLOAT: {
      std::optional<double> value = base::StringToDouble(text);
      if (!value) {
        return c->Unsupported(expr, "a malformed number");
      }
      out.type = ConditionLiteral::Type::kDouble;
      out.double_value = negate ? -*value : *value;
      break;
    }
    case SYNTAQLITE_LITERAL_TYPE_STRING:
      if (negate) {
        return c->Unsupported(expr, "a negated string");
      }
      out.type = ConditionLiteral::Type::kString;
      out.int_value = static_cast<int64_t>(strings->size());
      strings->push_back(StringLiteralText(text));
      break;
    default:
      return c->Unsupported(expr, "a literal other than a number or string");
  }
  literals->push_back(out);
  return base::OkStatus();
}

std::optional<core::Op> ComparisonOp(SyntaqliteBinaryOp op) {
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

base::StatusOr<core::Op> NullTestOp(Compiler* c,
                                    uint32_t expr,
                                    const SyntaqliteIsExpr& node) {
  switch (static_cast<int>(node.op)) {
    case SYNTAQLITE_IS_OP_IS_NULL:
      return core::Op(core::IsNull{});
    case SYNTAQLITE_IS_OP_NOT_NULL:
    case SYNTAQLITE_IS_OP_NOT_NULL_SPACED:
      return core::Op(core::IsNotNull{});
    case SYNTAQLITE_IS_OP_IS:
    case SYNTAQLITE_IS_OP_IS_NOT: {
      const auto* right = Node<SyntaqliteNode>(c->parser(), node.right);
      if (right->tag != SYNTAQLITE_NODE_LITERAL ||
          right->literal.literal_type != SYNTAQLITE_LITERAL_TYPE_NULL) {
        return c->Unsupported(expr, "IS other than IS [NOT] NULL");
      }
      return node.op == SYNTAQLITE_IS_OP_IS ? core::Op(core::IsNull{})
                                            : core::Op(core::IsNotNull{});
    }
    default:
      return c->Unsupported(expr, "IS other than IS [NOT] NULL");
  }
}

std::optional<std::string> LiteralMismatch(const core::Op& op,
                                           const ConditionLiteral* literals,
                                           uint32_t count,
                                           core::StorageType type) {
  if (op.IsAnyOf<core::NullOp>() || op.Is<core::Glob>()) {
    return std::nullopt;
  }
  bool strings = type.Is<core::String>();
  for (uint32_t i = 0; i < count; ++i) {
    if (strings != (literals[i].type == ConditionLiteral::Type::kString)) {
      return std::string(strings ? "a string to compare with a string column, "
                                   "but got a number"
                                 : "a number to compare with a numeric "
                                   "column, but got a string");
    }
  }
  return std::nullopt;
}

ex::FilterValue CastConditionLiterals(const core::Op& op,
                                      const ConditionLiteral* literals,
                                      uint32_t count,
                                      const std::vector<std::string>& strings,
                                      core::StorageType type,
                                      const StringPool& pool) {
  LiteralFetcher fetcher(literals, count, strings);
  ex::FilterValue value;
  if (op.Is<core::In>()) {
    ex::GetFilterValueListCaster<LiteralFetcher>(type)(fetcher, 0, pool,
                                                       &value);
  } else {
    ex::GetFilterValueCaster<LiteralFetcher>(type, op)(fetcher, 0, pool,
                                                       &value);
  }
  return value;
}

}  // namespace perfetto::trace_processor::pipeline
