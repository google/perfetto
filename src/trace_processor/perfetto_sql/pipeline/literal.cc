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

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/variant.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

// Hands literals to the shared filter value casts, so they mean for each column
// type what any other filter value means. A scalar is the first literal; IN
// iterates them all.
struct LiteralFetcher : core::ValueFetcher {
  static constexpr Type kInt64 = 0;
  static constexpr Type kDouble = 1;
  static constexpr Type kString = 2;
  static constexpr Type kNull = 3;

  LiteralFetcher(const Literal* l, uint32_t n) : literals(l), count(n) {}

  Type GetValueType(uint32_t) const {
    switch (literals[at].type) {
      case Literal::Type::kInt64:
        return kInt64;
      case Literal::Type::kDouble:
        return kDouble;
      case Literal::Type::kString:
        return kString;
    }
    PERFETTO_FATAL("Unknown literal type");
  }
  int64_t GetInt64Value(uint32_t) const { return literals[at].int_value; }
  double GetDoubleValue(uint32_t) const { return literals[at].double_value; }
  const char* GetStringValue(uint32_t) const {
    return literals[at].text.c_str();
  }
  bool IteratorInit(uint32_t) {
    at = 0;
    return count != 0;
  }
  bool IteratorNext(uint32_t) { return ++at < count; }

  const Literal* literals;
  uint32_t count;
  uint32_t at = 0;
};

// The text of 'it''s' is it's: the span keeps the quotes and doubles them.
std::string StringLiteralText(const std::string& source) {
  PERFETTO_CHECK(source.size() >= 2 && source.front() == '\'' &&
                 source.back() == '\'');
  return base::ReplaceAll(source.substr(1, source.size() - 2), "''", "'");
}

}  // namespace

base::StatusOr<Literal> CompileLiteral(Compiler* c, uint32_t expr) {
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
  Literal out;
  switch (static_cast<int>(literal.literal_type)) {
    case SYNTAQLITE_LITERAL_TYPE_INTEGER: {
      std::optional<int64_t> value = base::StringToInt64(text);
      if (!value && base::StartsWith(base::ToLower(text), "0x")) {
        std::optional<uint64_t> bits = base::StringToUInt64(text.substr(2), 16);
        if (bits) {
          value = static_cast<int64_t>(*bits);
        }
      }
      if (!value) {
        return c->Unsupported(expr, "a number which does not fit an integer");
      }
      out.type = Literal::Type::kInt64;
      out.int_value = negate ? -*value : *value;
      break;
    }
    case SYNTAQLITE_LITERAL_TYPE_FLOAT: {
      std::optional<double> value = base::StringToDouble(text);
      if (!value) {
        return c->Unsupported(expr, "a malformed number");
      }
      out.type = Literal::Type::kDouble;
      out.double_value = negate ? -*value : *value;
      break;
    }
    case SYNTAQLITE_LITERAL_TYPE_STRING:
      if (negate) {
        return c->Unsupported(expr, "a negated string");
      }
      out.type = Literal::Type::kString;
      out.text = StringLiteralText(text);
      break;
    default:
      return c->Unsupported(expr, "a literal other than a number or string");
  }
  return out;
}

ex::FilterValue CastLiterals(const core::Op& op,
                             const Literal* literals,
                             uint32_t count,
                             core::StorageType type,
                             const StringPool& pool) {
  LiteralFetcher fetcher(literals, count);
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

ResolvedComparison ResolveLiteralComparison(const core::Op& op,
                                            const Literal& literal,
                                            bool double_value) {
  LiteralFetcher fetcher(&literal, 1);
  core::NonNullOp non_null = *op.TryDowncast<core::NonNullOp>();
  core::filter::CastFilterValueResult cast =
      double_value
          ? core::filter::CastFilterValue<core::Double>(0, fetcher, non_null)
          : core::filter::CastFilterValue<core::Int64>(0, fetcher, non_null);
  ResolvedComparison resolved;
  switch (cast.validity) {
    case core::filter::CastFilterValueResult::kAllMatch:
      resolved.kind = ResolvedComparison::Kind::kAlwaysTrue;
      return resolved;
    case core::filter::CastFilterValueResult::kNoneMatch:
      resolved.kind = ResolvedComparison::Kind::kAlwaysFalse;
      return resolved;
    case core::filter::CastFilterValueResult::kValid:
      break;
  }
  if (double_value) {
    resolved.literal.type = Literal::Type::kDouble;
    resolved.literal.double_value = base::unchecked_get<double>(cast.value);
  } else {
    resolved.literal.int_value = base::unchecked_get<int64_t>(cast.value);
  }
  return resolved;
}

void SerializeLiteral(PlanCodec& c, Literal* literal) {
  c.Pod(&literal->type);
  c.Pod(&literal->int_value);
  c.Pod(&literal->double_value);
  c.Str(&literal->text);
}

}  // namespace perfetto::trace_processor::pipeline
