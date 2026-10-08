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

#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"

namespace perfetto::trace_processor::pipeline {

void Writer::U8(uint8_t value) {
  Append(&value, sizeof(value));
}

void Writer::U32(uint32_t value) {
  Append(&value, sizeof(value));
}

void Writer::Size(size_t value) {
  U32(static_cast<uint32_t>(value));
}

void Writer::Str(std::string_view value) {
  Size(value.size());
  out_.append(value.data(), value.size());
}

void Writer::Position(const Available& available, ColumnId id) {
  auto it = std::find(available.begin(), available.end(), id);
  Size(static_cast<size_t>(it - available.begin()));
}

std::string Writer::Take() {
  return std::move(out_);
}

void Writer::Append(const void* data, size_t size) {
  size_t at = out_.size();
  out_.resize(at + size);
  memcpy(out_.data() + at, data, size);
}

Reader::Reader(std::string_view in) : in_(in) {}

uint8_t Reader::U8() {
  uint8_t value = 0;
  Read(&value, sizeof(value));
  return value;
}

uint32_t Reader::U32() {
  uint32_t value = 0;
  Read(&value, sizeof(value));
  return value;
}

uint32_t Reader::Count() {
  uint32_t count = U32();
  if (count > in_.size() - at_) {
    Fail();
    return 0;
  }
  return count;
}

std::string Reader::Str() {
  uint32_t size = Count();
  std::string value(in_.substr(at_, size));
  at_ += size;
  return value;
}

ColumnId Reader::Position(const Available& available) {
  uint32_t i = U32();
  if (i >= available.size()) {
    Fail();
    return 0;
  }
  return available[i];
}

void Reader::Fail() {
  ok_ = false;
}

bool Reader::ok() const {
  return ok_;
}

bool Reader::done() const {
  return ok_ && at_ == in_.size();
}

void Reader::Read(void* out, size_t size) {
  if (!ok_ || in_.size() - at_ < size) {
    Fail();
    return;
  }
  memcpy(out, in_.data() + at_, size);
  at_ += size;
}

PlanWriter::PlanWriter(const LogicalPlan& plan) : plan_(plan) {}

PlanReader::PlanReader(Reader& r) : r_(r) {}

void WriteType(Writer* w, const std::optional<core::StorageType>& type) {
  // Zero is a type known only per row; otherwise the type's index plus one.
  w->U8(type ? static_cast<uint8_t>(type->index() + 1) : 0);
}

std::optional<core::StorageType> ReadType(Reader* r) {
  uint8_t tag = r->U8();
  if (tag == 0) {
    return std::nullopt;
  }
  switch (tag - 1) {
    case core::StorageType::GetTypeIndex<core::Id>():
      return core::Id{};
    case core::StorageType::GetTypeIndex<core::Uint32>():
      return core::Uint32{};
    case core::StorageType::GetTypeIndex<core::Int32>():
      return core::Int32{};
    case core::StorageType::GetTypeIndex<core::Int64>():
      return core::Int64{};
    case core::StorageType::GetTypeIndex<core::Double>():
      return core::Double{};
    case core::StorageType::GetTypeIndex<core::String>():
      return core::String{};
    default:
      r->Fail();
      return std::nullopt;
  }
}

// A plan is written as its source then the stages above it, naming columns by
// position among those available, so only plans the compiler could build can
// be written and reading one needs only bounds checks.
std::string PlanWriter::Write() {
  // Pruning can leave nodes the root does not reach.
  std::vector<const PlanNode*> stages;
  PlanNodeId id = plan_.root();
  while (
      !plan_.nodes()[id].operation().registration().encoding()->is_source()) {
    stages.push_back(&plan_.nodes()[id]);
    // A stage's input is its first child; it writes any others it reads, as
    // FILL GAPS does its background.
    PERFETTO_CHECK(!plan_.nodes()[id].children().empty());
    id = plan_.nodes()[id].children()[0];
  }
  const PlanNode& source = plan_.nodes()[id];
  w_.U8(source.operation().registration().encoding()->tag());
  Available available;
  source.operation().Write(this, source, &available);
  w_.Size(stages.size());
  for (auto it = stages.rbegin(); it != stages.rend(); ++it) {
    const PlanNode& stage = **it;
    w_.U8(stage.operation().registration().encoding()->tag());
    stage.operation().Write(this, stage, &available);
  }
  w_.Size(plan_.output().size());
  for (const NamedColumn& column : plan_.output()) {
    w_.Str(column.name);
    w_.Position(available, column.id);
  }
  return w_.Take();
}

ColumnId PlanReader::AddColumn(std::string name,
                               std::optional<core::StorageType> type) {
  return plan_.AddColumn(std::move(name), type);
}

LogicalPlan PlanReader::Read() {
  Available available;
  auto read = [&](bool source) {
    const auto* registration = FindOperationByTag(r_.U8());
    if (!registration || registration->encoding()->is_source() != source) {
      r_.Fail();
      return;
    }
    registration->encoding()->DecodePlan(this, &available);
  };
  read(true);
  uint32_t stages = r_.Count();
  for (uint32_t i = 0; i < stages && r_.ok(); ++i) {
    read(false);
  }
  // The table function declares only so many output columns.
  uint32_t outputs = r_.Count();
  if (outputs > kMaxPipelineColumns) {
    r_.Fail();
    return {};
  }
  plan_.output().resize(outputs);
  for (NamedColumn& column : plan_.output()) {
    column.name = r_.Str();
    column.id = r_.Position(available);
  }
  return std::move(plan_);
}

namespace {

// Points each dataframe scan at the dataframe now registered under its name,
// which must still have every column the plan reads, with the same type.
base::Status ResolveDataframes(LogicalPlan& plan, const Catalog& catalog) {
  for (PlanNode& node : plan.nodes()) {
    RETURN_IF_ERROR(node.operation().ResolveDataframes(&plan, catalog));
  }
  return base::OkStatus();
}

}  // namespace

std::string SerializePlan(const LogicalPlan& plan) {
  return PlanWriter(plan).Write();
}

base::Status BindDataframeArgs(
    LogicalPlan& plan,
    const std::vector<const dataframe::Dataframe*>& args,
    StringPool* pool) {
  for (PlanNode& node : plan.nodes()) {
    RETURN_IF_ERROR(node.operation().BindDataframeArgs(&plan, args, pool));
  }
  return base::OkStatus();
}

base::StatusOr<LogicalPlan> DeserializePlan(std::string_view bytes,
                                            const Catalog& catalog) {
  Reader r(bytes);
  LogicalPlan plan = PlanReader(r).Read();
  if (!r.done()) {
    return base::ErrStatus("__intrinsic_pipeline: malformed plan");
  }
  RETURN_IF_ERROR(ResolveDataframes(plan, catalog));
  return std::move(plan);
}

}  // namespace perfetto::trace_processor::pipeline
