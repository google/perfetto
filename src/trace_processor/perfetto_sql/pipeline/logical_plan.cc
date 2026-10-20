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

#include <string>
#include <string_view>

#include "perfetto/ext/base/string_utils.h"

namespace perfetto::trace_processor::pipeline {

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

PlanNode::PlanNode(std::unique_ptr<PlanOperation> operation,
                   std::vector<PlanNodeId> inputs)
    : children_(std::move(inputs)), operation_(std::move(operation)) {}

PlanNode::~PlanNode() = default;

PlanNode::PlanNode(const PlanNode& other)
    : children_(other.children()), operation_(other.operation_->Clone()) {}

PlanNode& PlanNode::operator=(const PlanNode& other) {
  if (this != &other) {
    children_ = other.children();
    operation_ = other.operation_->Clone();
  }
  return *this;
}

PlanNode::PlanNode(PlanNode&&) noexcept = default;
PlanNode& PlanNode::operator=(PlanNode&&) noexcept = default;

PlanOperation::~PlanOperation() = default;

void PlanOperation::MoveSqlSourcesToDataframeArgs(const NameTable&,
                                                  std::vector<std::string>*) {}

base::Status PlanOperation::ResolveDataframes(LogicalPlan*, const Catalog&) {
  return base::OkStatus();
}

base::Status PlanOperation::BindDataframeArgs(
    LogicalPlan*,
    const std::vector<const core::dataframe::Dataframe*>&,
    StringPool*) {
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::pipeline
