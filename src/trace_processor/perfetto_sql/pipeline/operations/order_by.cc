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

#include "src/trace_processor/perfetto_sql/pipeline/operations/order_by.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {

const OperationRegistration OrderBy::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_ORDER_BY, &BuildPlan,
    OperationRegistration::Encoding{4, false, &DecodePlan}};

base::Status OrderBy::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("ORDER BY");
  const auto* n = Node<SyntaqlitePerfettoPipeOrderBy>(c->parser(), stage);
  const auto* terms =
      Node<SyntaqlitePerfettoPipeOrderTermList>(c->parser(), n->terms);

  OrderBy order;
  for (uint32_t i = 0; i < syntaqlite_list_count(terms); ++i) {
    uint32_t term_id = syntaqlite_list_child_id(terms, i);
    const auto* term =
        Node<SyntaqlitePerfettoPipeOrderTerm>(c->parser(), term_id);
    ASSIGN_OR_RETURN(ColumnId id, c->ResolveColumnExpr(term->expr));
    const auto& type = c->plan().columns()[id].type;
    if (type && type->Is<core::String>()) {
      return c->Unsupported(term->expr, "ordering by a string column");
    }
    order.keys_.push_back({id, term->sort_order == SYNTAQLITE_SORT_ORDER_DESC});
  }
  c->AddNode(std::move(order), {c->plan().root()});
  return base::OkStatus();
}

std::optional<uint32_t> OrderBy::Prune(std::vector<bool>* needed) {
  for (const Key& key : keys_) {
    (*needed)[key.column] = true;
  }
  return std::nullopt;
}

void OrderBy::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerNode(node.children()[0]);
  std::vector<Lowering::SortKey> keys;
  for (const Key& key : keys_) {
    keys.push_back({key.column, key.descending});
  }
  c->SortRows(keys);
}

const OperationRegistration& OrderBy::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> OrderBy::Clone() const {
  return std::make_unique<OrderBy>(*this);
}

void OrderBy::Write(PlanWriter* c,
                    const PlanNode&,
                    Available* available) const {
  c->writer().Size(keys_.size());
  for (const Key& key : keys_) {
    c->writer().Position(*available, key.column);
    c->writer().U8(key.descending);
  }
}

void OrderBy::DecodePlan(PlanReader* c, Available* available) {
  OrderBy order;
  order.keys_.resize(c->reader().Count());
  for (Key& key : order.keys_) {
    key.column = c->reader().Position(*available);
    key.descending = c->reader().U8() != 0;
  }
  c->AddNode(std::move(order), {c->plan().root()});
}

}  // namespace perfetto::trace_processor::pipeline
