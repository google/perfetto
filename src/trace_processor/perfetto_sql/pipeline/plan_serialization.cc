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

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"

namespace perfetto::trace_processor::pipeline {
namespace {

// Binds each dataframe scan to the dataframe now registered under its name,
// which must still have every column the plan reads, with the same type.
base::Status ResolveDataframes(BoundPlan* bound,
                               const Catalog& catalog,
                               const PlanKey* key) {
  const LogicalPlan& plan = bound->plan;
  for (uint32_t i = 0; i < plan.nodes().size(); ++i) {
    RETURN_IF_ERROR(plan.nodes()[i].operation().ResolveDataframes(
        plan, i, catalog, key, &bound->bindings));
  }
  return base::OkStatus();
}

}  // namespace

PERFETTO_NO_INLINE void PlanCodec::Bytes(void* data, size_t size) {
  // An empty vector's data can be null, which memcpy may not be given.
  if (size == 0) {
    return;
  }
  if (out_) {
    out_->append(static_cast<const char*>(data), size);
    return;
  }
  PERFETTO_DCHECK(size <= in_.size());
  memcpy(data, in_.data(), size);
  in_.remove_prefix(size);
}

PERFETTO_NO_INLINE uint32_t PlanCodec::Count(size_t size) {
  auto count = static_cast<uint32_t>(size);
  Pod(&count);
  return count;
}

void PlanCodec::Str(std::string* value) {
  value->resize(Count(value->size()));
  Bytes(value->data(), value->size());
}

namespace {

// A plan as it is held: its names, its columns, each node as its tag,
// children and payload, then its root and outputs.
void SerializeContents(PlanCodec& c) {
  LogicalPlan& plan = c.plan();
  uint32_t names = c.Count(plan.names().size());
  PERFETTO_NO_VECTORIZE_LOOP
  for (NameId i = 0; i < names; ++i) {
    std::string text = c.reading() ? std::string() : plan.names().text(i);
    c.Str(&text);
    if (c.reading()) {
      plan.names().Intern(text);
    }
  }
  c.Vec(&plan.columns());
  uint32_t nodes = c.Count(plan.nodes().size());
  PERFETTO_NO_VECTORIZE_LOOP
  for (PlanNodeId i = 0; i < nodes; ++i) {
    uint8_t tag = 0;
    std::vector<PlanNodeId> children;
    if (!c.reading()) {
      tag = plan.nodes()[i].operation().registration().encoding()->tag();
      children = plan.nodes()[i].children();
    }
    c.Pod(&tag);
    c.Vec(&children);
    if (c.reading()) {
      plan.AddNode(FindOperationByTag(tag)->encoding()->Make(),
                   std::move(children));
    }
    plan.nodes()[i].operation().Serialize(c);
  }
  PlanNodeId root = plan.root();
  c.Pod(&root);
  plan.SetRoot(root);
  c.Vec(&plan.output());
}

}  // namespace

std::string SerializePlan(LogicalPlan plan, const PlanKey& key) {
  std::string out;
  PlanCodec c(&plan, &key, &out);
  SerializeContents(c);
  uint64_t stamp = key.Stamp(PlanKey::Use::kPlan, out);
  c.Pod(&stamp);
  return out;
}

base::Status BindDataframeArgs(
    BoundPlan* bound,
    const std::vector<const dataframe::Dataframe*>& args,
    StringPool* pool) {
  const LogicalPlan& plan = bound->plan;
  for (uint32_t i = 0; i < plan.nodes().size(); ++i) {
    RETURN_IF_ERROR(plan.nodes()[i].operation().BindDataframeArgs(
        plan, i, args, pool, &bound->bindings));
  }
  return base::OkStatus();
}

base::StatusOr<BoundPlan> ResolveCompiledPlan(LogicalPlan plan,
                                              const Catalog& catalog) {
  BoundPlan bound(std::move(plan));
  RETURN_IF_ERROR(ResolveDataframes(&bound, catalog, nullptr));
  return std::move(bound);
}

base::StatusOr<BoundPlan> DeserializePlan(std::string_view bytes,
                                          const PlanKey& key,
                                          const Catalog& catalog) {
  // Bytes in SQL could be anything: only those this process stamped are read.
  uint64_t stamp = 0;
  bool stamped = bytes.size() > sizeof(stamp);
  if (stamped) {
    memcpy(&stamp, bytes.data() + bytes.size() - sizeof(stamp), sizeof(stamp));
    bytes.remove_suffix(sizeof(stamp));
  }
  if (!stamped || key.Stamp(PlanKey::Use::kPlan, bytes) != stamp) {
    return base::ErrStatus("__intrinsic_pipeline: malformed plan");
  }
  LogicalPlan plan;
  PlanCodec c(&plan, &key, bytes);
  SerializeContents(c);
  BoundPlan bound(std::move(plan));
  RETURN_IF_ERROR(ResolveDataframes(&bound, catalog, &key));
  return std::move(bound);
}

}  // namespace perfetto::trace_processor::pipeline
