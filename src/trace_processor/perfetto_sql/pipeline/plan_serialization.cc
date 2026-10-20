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
#include <memory>
#include <optional>
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
namespace {

// Writes a plan's payloads as they are given.
class PlanWriter final : public PlanCodec {
 public:
  explicit PlanWriter(LogicalPlan* plan) : PlanCodec(plan, false) {}

  void U8(uint8_t* value) override { Append(value, sizeof(*value)); }
  void U32(uint32_t* value) override { Append(value, sizeof(*value)); }
  void U64(uint64_t* value) override { Append(value, sizeof(*value)); }
  void F64(double* value) override { Append(value, sizeof(*value)); }
  void Str(std::string* value) override {
    Count(value->size());
    out_.append(*value);
  }
  void Name(NameId* name) override { U32(name); }
  void Column(const Available& available, ColumnId* id) override {
    auto at = static_cast<uint32_t>(
        std::find(available.begin(), available.end(), *id) - available.begin());
    U32(&at);
  }
  uint32_t Count(size_t size) override {
    auto count = static_cast<uint32_t>(size);
    U32(&count);
    return count;
  }
  void Check(bool ok) override { PERFETTO_DCHECK(ok); }
  bool ok() const override { return true; }

  std::string Take() { return std::move(out_); }

 private:
  void Append(const void* data, size_t size) {
    out_.append(static_cast<const char*>(data), size);
  }

  std::string out_;
};

// Reads a plan's payloads, checking each field.
class PlanReader final : public PlanCodec {
 public:
  PlanReader(std::string_view in, LogicalPlan* plan)
      : PlanCodec(plan, true), in_(in) {}

  void U8(uint8_t* value) override { Read(value, sizeof(*value)); }
  void U32(uint32_t* value) override { Read(value, sizeof(*value)); }
  void U64(uint64_t* value) override { Read(value, sizeof(*value)); }
  void F64(double* value) override { Read(value, sizeof(*value)); }
  void Str(std::string* value) override {
    uint32_t size = Count(0);
    *value = std::string(in_.substr(at_, size));
    at_ += size;
  }
  void Name(NameId* name) override {
    U32(name);
    Check(*name < plan().names().size());
  }
  void Column(const Available& available, ColumnId* id) override {
    uint32_t at = 0;
    U32(&at);
    Check(at < available.size());
    *id = ok_ ? available[at] : 0;
  }
  uint32_t Count(size_t) override {
    uint32_t count = 0;
    U32(&count);
    Check(count <= in_.size() - at_);
    return ok_ ? count : 0;
  }
  void Check(bool ok) override { ok_ = ok_ && ok; }

  bool ok() const override { return ok_; }
  bool done() const { return ok_ && at_ == in_.size(); }

 private:
  void Read(void* out, size_t size) {
    Check(in_.size() - at_ >= size);
    if (!ok_) {
      memset(out, 0, size);
      return;
    }
    memcpy(out, in_.data() + at_, size);
    at_ += size;
  }

  std::string_view in_;
  size_t at_ = 0;
  bool ok_ = true;
};

// Tags for types that are not storage types. A storage type's tag is its
// index plus kFirstStorageTag.
constexpr uint8_t kUnknownTag = 0;
constexpr uint8_t kNullTag = 1;
constexpr uint8_t kFirstStorageTag = 2;

// Points each dataframe scan at the dataframe now registered under its name,
// which must still have every column the plan reads, with the same type.
base::Status ResolveDataframes(LogicalPlan& plan, const Catalog& catalog) {
  for (PlanNode& node : plan.nodes()) {
    RETURN_IF_ERROR(node.operation().ResolveDataframes(&plan, catalog));
  }
  return base::OkStatus();
}

}  // namespace

PlanCodec::~PlanCodec() = default;

void PlanCodec::Bool(bool* value) {
  uint8_t raw = *value;
  U8(&raw);
  Check(raw <= 1);
  *value = raw != 0;
}

void PlanCodec::Columns(const Available& available,
                        std::vector<ColumnId>* columns) {
  columns->resize(Count(columns->size()));
  for (ColumnId& column : *columns) {
    Column(available, &column);
  }
}

void PlanCodec::Type(PlanType* type) {
  uint8_t tag = kUnknownTag;
  if (type->is_null()) {
    tag = kNullTag;
  } else if (type->storage()) {
    tag = static_cast<uint8_t>(type->storage()->index() + kFirstStorageTag);
  }
  U8(&tag);
  if (tag == kNullTag) {
    *type = PlanType::Null();
  } else if (tag == kUnknownTag) {
    *type = PlanType::Unknown();
  } else {
    std::optional<core::StorageType> storage =
        core::StorageType::FromIndex(tag - kFirstStorageTag);
    Check(storage.has_value());
    *type = storage ? PlanType(*storage) : PlanType::Unknown();
  }
}

void PlanCodec::NewColumn(ColumnId* id, const PlanType& type) {
  NameId name = reading() ? 0 : plan().columns()[*id].name;
  Name(&name);
  if (reading()) {
    *id = plan().AddColumn(name, type);
  }
}

// A plan is written as its names, its source, then the stages above it,
// naming columns by position among those available, so only plans the
// compiler could build can be written and reading one needs only bounds
// checks.
std::string SerializePlan(LogicalPlan plan) {
  PlanWriter w(&plan);
  // Pruning can leave nodes the root does not reach.
  std::vector<PlanNodeId> chain;
  for (PlanNodeId id = plan.root();;) {
    chain.push_back(id);
    if (plan.nodes()[id].operation().registration().encoding()->is_source()) {
      break;
    }
    // A stage's input is its first child. The stage writes any other children
    // itself, like FILL GAPS writes its background.
    PERFETTO_CHECK(!plan.nodes()[id].children().empty());
    id = plan.nodes()[id].children()[0];
  }
  w.Count(plan.names().size());
  for (NameId name = 0; name < plan.names().size(); ++name) {
    std::string text = plan.names().text(name);
    w.Str(&text);
  }
  w.Count(chain.size() - 1);
  Available available;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    PlanNode& node = plan.nodes()[*it];
    uint8_t tag = node.operation().registration().encoding()->tag();
    w.U8(&tag);
    std::vector<PlanNodeId> children = node.children();
    node.operation().Serialize(w, &children, &available);
  }
  w.Count(plan.output().size());
  for (NamedColumn& column : plan.output()) {
    w.Name(&column.name);
    w.Column(available, &column.id);
  }
  return w.Take();
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
  LogicalPlan plan;
  PlanReader r(bytes, &plan);
  // A writer interns each spelling once, so a repeated one is corrupt: it
  // would give the names after it different ids.
  uint32_t names = r.Count(0);
  for (uint32_t i = 0; i < names; ++i) {
    std::string text;
    r.Str(&text);
    r.Check(plan.names().Intern(text) == i);
  }
  uint32_t stages = r.Count(0);
  Available available;
  for (uint32_t i = 0; i <= stages && r.ok(); ++i) {
    uint8_t tag = 0;
    r.U8(&tag);
    const auto* registration = FindOperationByTag(tag);
    // The first is the source; the rest are stages.
    r.Check(registration && registration->encoding()->is_source() == (i == 0));
    if (!r.ok()) {
      break;
    }
    std::unique_ptr<PlanOperation> operation = registration->encoding()->Make();
    std::vector<PlanNodeId> children;
    if (i != 0) {
      children.push_back(plan.root());
    }
    operation->Serialize(r, &children, &available);
    plan.AddNode(std::move(operation), std::move(children));
  }
  // The table function declares only so many output columns.
  uint32_t outputs = r.Count(0);
  r.Check(outputs <= kMaxPipelineColumns);
  plan.output().resize(r.ok() ? outputs : 0);
  for (NamedColumn& column : plan.output()) {
    r.Name(&column.name);
    r.Column(available, &column.id);
  }
  if (!r.done()) {
    return base::ErrStatus("__intrinsic_pipeline: malformed plan");
  }
  RETURN_IF_ERROR(ResolveDataframes(plan, catalog));
  return std::move(plan);
}

}  // namespace perfetto::trace_processor::pipeline
