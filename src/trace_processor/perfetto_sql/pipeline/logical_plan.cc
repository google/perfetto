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

#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/core/common/storage_types.h"

namespace perfetto::trace_processor::pipeline {

std::optional<PlanType> CommonType(const PlanType& a, const PlanType& b) {
  if (a.is_null()) {
    return b;
  }
  if (b.is_null()) {
    return a;
  }
  if (a.is_unknown() && b.is_unknown()) {
    return a;
  }
  if (a.is_unknown() || b.is_unknown()) {
    const PlanType& known = a.is_unknown() ? b : a;
    if (known.storage()->IsAnyOf<core::IntegerType>()) {
      return PlanType(core::Int64{});
    }
    return known;
  }
  core::StorageType x = *a.storage();
  core::StorageType y = *b.storage();
  if (x == y) {
    return a;
  }
  if (x.Is<core::String>() || y.Is<core::String>()) {
    return std::nullopt;
  }
  if (x.Is<core::Double>() || y.Is<core::Double>()) {
    return PlanType(core::Double{});
  }
  return PlanType(core::Int64{});
}

// A plan has tens of names, so a scan beats a hash table, in code as in time.
NameId NameTable::Intern(std::string_view text) {
  auto id = static_cast<NameId>(texts_.size());
  NameId key = id;
  for (NameId i = 0; i < texts_.size(); ++i) {
    if (texts_[i] == text) {
      return i;
    }
    if (key == id && base::CaseInsensitiveEqual(texts_[i], text)) {
      key = keys_[i];
    }
  }
  texts_.emplace_back(text);
  keys_.push_back(key);
  return id;
}

void LogicalPlan::Bypass(PlanNodeId node) {
  PlanNodeId input = nodes_[node].children()[0];
  for (PlanNode& other : nodes_) {
    for (PlanNodeId& child : other.children()) {
      if (child == node) {
        child = input;
      }
    }
  }
  if (root_ == node) {
    root_ = input;
  }
}

void PlanOperation::MoveSqlSourcesToDataframeArgs(const NameTable&,
                                                  std::vector<std::string>*) {}

void PlanOperation::Rewrite(BoundPlan*,
                            PlanNodeId,
                            const LoweringContext&) const {}

InputNeeds PlanOperation::Needs(uint32_t) const {
  return {};
}

std::vector<std::vector<InputColumn>> PlanOperation::CommonTypes() const {
  return {};
}

base::Status PlanOperation::ResolveDataframes(const LogicalPlan&,
                                              PlanNodeId,
                                              const Catalog*,
                                              const PlanKey*,
                                              Bindings*) const {
  return base::OkStatus();
}

base::Status PlanOperation::BindDataframeArgs(
    const LogicalPlan&,
    PlanNodeId,
    base::Span<const core::dataframe::Dataframe* const>,
    StringPool*,
    Bindings*) const {
  return base::OkStatus();
}

BoundPlan::BoundPlan(LogicalPlan p) : plan(std::move(p)) {
  bindings.dataframes.resize(plan.nodes().size());
  bindings.types.reserve(plan.columns().size());
  for (const PlanColumn& column : plan.columns()) {
    bindings.types.push_back(column.type);
  }
}

}  // namespace perfetto::trace_processor::pipeline
