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

#include "src/trace_processor/perfetto_sql/pipeline/operations/limit.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/exec/limit.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

// Reads an integer literal, optionally negated.
base::StatusOr<int64_t> ParseInteger(Compiler* c, uint32_t expr) {
  SyntaqliteParser* p = c->parser();
  const auto* node = Node<SyntaqliteNode>(p, expr);
  bool negate = false;
  if (node->tag == SYNTAQLITE_NODE_UNARY_EXPR &&
      node->unary_expr.op == SYNTAQLITE_UNARY_OP_MINUS) {
    negate = true;
    node = Node<SyntaqliteNode>(p, node->unary_expr.operand);
  }
  if (node->tag != SYNTAQLITE_NODE_LITERAL ||
      node->literal.literal_type != SYNTAQLITE_LITERAL_TYPE_INTEGER) {
    return c->Unsupported(expr, "a value other than an integer literal");
  }
  std::optional<int64_t> value =
      base::StringToInt64(SpanText(p, node->literal.source));
  if (!value) {
    return c->Unsupported(expr, "a number which does not fit an integer");
  }
  return negate ? -*value : *value;
}

// Batches count rows in uint32_t, so more than that many is as good as all.
uint32_t Clamp(int64_t value) {
  return static_cast<uint32_t>(
      std::min<int64_t>(value, std::numeric_limits<uint32_t>::max()));
}

}  // namespace

const OperationRegistration Limit::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_LIMIT, &BuildPlan,
    OperationRegistration::Encoding{11, &MakeOperation<Limit>}};

base::Status Limit::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("LIMIT");
  const auto* n = Node<SyntaqlitePerfettoPipeLimit>(c->parser(), stage);
  Limit limit;
  ASSIGN_OR_RETURN(limit.count_, ParseInteger(c, n->count));
  if (syntaqlite_node_is_present(n->offset)) {
    ASSIGN_OR_RETURN(limit.offset_, ParseInteger(c, n->offset));
  }
  c->AddNode(std::move(limit), {c->plan().root()});
  return base::OkStatus();
}

std::optional<uint32_t> Limit::Prune(std::vector<bool>*) {
  return std::nullopt;
}

void Limit::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerNode(node.children()[0]);
  std::optional<uint32_t> count;
  if (count_ >= 0) {
    count = Clamp(count_);
  }
  // Rows kept stay in their input order, so any known arrangement survives.
  c->AddOperator(
      std::make_unique<ex::Limit>(count, Clamp(std::max<int64_t>(offset_, 0))));
  c->DeclareSmallBatches();
  c->DeclareRowsDropped();
}

const OperationRegistration& Limit::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> Limit::Clone() const {
  return std::make_unique<Limit>(*this);
}

void Limit::Serialize(PlanCodec& c) {
  c.Pod(&count_);
  c.Pod(&offset_);
}

}  // namespace perfetto::trace_processor::pipeline
