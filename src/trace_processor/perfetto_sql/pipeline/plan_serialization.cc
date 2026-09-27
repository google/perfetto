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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {
namespace {

// Bumped whenever the layout changes. Plans never leave the process which
// wrote them, so no older layout is read.
constexpr uint32_t kVersion = 1;

class Writer {
 public:
  void U8(uint8_t value) { Append(&value, sizeof(value)); }
  void U32(uint32_t value) { Append(&value, sizeof(value)); }
  void Size(size_t value) { U32(static_cast<uint32_t>(value)); }
  void Str(std::string_view value) {
    Size(value.size());
    out_.append(value.data(), value.size());
  }
  void Ids(const std::vector<ColumnId>& ids) {
    Size(ids.size());
    for (ColumnId id : ids) {
      U32(id);
    }
  }
  std::string Take() { return std::move(out_); }

 private:
  void Append(const void* data, size_t size) {
    size_t at = out_.size();
    out_.resize(at + size);
    memcpy(out_.data() + at, data, size);
  }

  std::string out_;
};

// Reads what Writer wrote. The first thing which does not fit marks the whole
// read as failed; every read after that returns zeroes.
class Reader {
 public:
  explicit Reader(std::string_view in) : in_(in) {}

  uint8_t U8() {
    uint8_t value = 0;
    Read(&value, sizeof(value));
    return value;
  }
  uint32_t U32() {
    uint32_t value = 0;
    Read(&value, sizeof(value));
    return value;
  }
  // A count of things at least a byte long each, so never more than the bytes
  // left: a corrupt count cannot make us allocate much.
  uint32_t Count() {
    uint32_t count = U32();
    if (count > in_.size() - at_) {
      Fail();
      return 0;
    }
    return count;
  }
  std::string Str() {
    uint32_t size = Count();
    std::string value(in_.substr(at_, size));
    at_ += size;
    return value;
  }
  std::vector<ColumnId> Ids() {
    std::vector<ColumnId> ids(Count());
    for (ColumnId& id : ids) {
      id = U32();
    }
    return ids;
  }

  void Fail() { ok_ = false; }
  bool done() const { return ok_ && at_ == in_.size(); }

 private:
  void Read(void* out, size_t size) {
    if (!ok_ || in_.size() - at_ < size) {
      Fail();
      return;
    }
    memcpy(out, in_.data() + at_, size);
    at_ += size;
  }

  std::string_view in_;
  size_t at_ = 0;
  bool ok_ = true;
};

void WriteScan(Writer& w, const op::Scan& scan) {
  using Kind = op::Scan::SourceKind;
  w.U8(static_cast<uint8_t>(scan.source.index()));
  switch (scan.source.index()) {
    case Kind::GetTypeIndex<op::Scan::Dataframe>():
      // Its columns are looked up again when read.
      w.Str(base::unchecked_get<op::Scan::Dataframe>(scan.source).name);
      break;
    case Kind::GetTypeIndex<op::Scan::DataframeArg>():
      w.U32(base::unchecked_get<op::Scan::DataframeArg>(scan.source).index);
      break;
    default:
      // SQL is moved out into dataframe arguments before a plan is written.
      PERFETTO_FATAL("Unknown scan source");
  }
  w.Size(scan.columns.size());
  for (const NamedColumn& column : scan.columns) {
    w.Str(column.name);
    w.U32(column.id);
  }
}

void WriteTreeAccumulate(Writer& w, const op::TreeAccumulate& acc) {
  w.U8(static_cast<uint8_t>(acc.direction));
  w.U32(acc.node_column);
  w.U32(acc.parent_column);
  w.Size(acc.aggregates.size());
  for (const op::TreeAccumulate::Aggregate& agg : acc.aggregates) {
    w.U8(static_cast<uint8_t>(agg.function));
    w.U32(agg.column);
    w.U32(agg.output);
  }
}

void WriteIntervalIntersect(Writer& w, const op::IntervalIntersect& isect) {
  w.U32(isect.ts);
  w.U32(isect.dur);
  w.Size(isect.operands.size());
  for (const op::IntervalIntersect::Operand& operand : isect.operands) {
    w.U32(operand.ts);
    w.U32(operand.dur);
    w.Ids(operand.keys);
    w.Ids(operand.carried);
  }
}

std::optional<core::StorageType> ReadType(Reader& r) {
  // Zero is a type known only per row; otherwise the type's index plus one.
  uint8_t tag = r.U8();
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
      r.Fail();
      return std::nullopt;
  }
}

op::Scan ReadScan(Reader& r) {
  using Kind = op::Scan::SourceKind;
  op::Scan scan;
  switch (r.U8()) {
    case Kind::GetTypeIndex<op::Scan::Dataframe>(): {
      op::Scan::Dataframe source;
      source.name = r.Str();
      scan.source = std::move(source);
      break;
    }
    case Kind::GetTypeIndex<op::Scan::DataframeArg>():
      scan.source = op::Scan::DataframeArg{r.U32()};
      break;
    default:
      r.Fail();
      return scan;
  }
  scan.columns.resize(r.Count());
  for (NamedColumn& column : scan.columns) {
    column.name = r.Str();
    column.id = r.U32();
  }
  return scan;
}

op::TreeAccumulate ReadTreeAccumulate(Reader& r) {
  op::TreeAccumulate acc;
  uint8_t direction = r.U8();
  if (direction > static_cast<uint8_t>(op::TreeDirection::kDown)) {
    r.Fail();
  }
  acc.direction = static_cast<op::TreeDirection>(direction);
  acc.node_column = r.U32();
  acc.parent_column = r.U32();
  acc.aggregates.resize(r.Count());
  for (op::TreeAccumulate::Aggregate& agg : acc.aggregates) {
    if (r.U8() != static_cast<uint8_t>(op::TreeAccumulate::Function::kSum)) {
      r.Fail();
    }
    agg.column = r.U32();
    agg.output = r.U32();
  }
  return acc;
}

op::IntervalIntersect ReadIntervalIntersect(Reader& r) {
  op::IntervalIntersect isect;
  isect.ts = r.U32();
  isect.dur = r.U32();
  isect.operands.resize(r.Count());
  for (op::IntervalIntersect::Operand& operand : isect.operands) {
    operand.ts = r.U32();
    operand.dur = r.U32();
    operand.keys = r.Ids();
    operand.carried = r.Ids();
  }
  return isect;
}

PlanNode ReadNode(Reader& r) {
  PlanNode node;
  uint8_t kind = r.U8();
  node.children.resize(r.Count());
  for (PlanNodeId& child : node.children) {
    child = r.U32();
  }
  switch (kind) {
    case OpKind::GetTypeIndex<op::Scan>():
      node.op = ReadScan(r);
      break;
    case OpKind::GetTypeIndex<op::TreeAccumulate>():
      node.op = ReadTreeAccumulate(r);
      break;
    case OpKind::GetTypeIndex<op::IntervalIntersect>():
      node.op = ReadIntervalIntersect(r);
      break;
    default:
      r.Fail();
      break;
  }
  return node;
}

base::Status Malformed() {
  return base::ErrStatus("__intrinsic_pipeline: malformed plan");
}

// Checks a plan is one lowering can run as it would a compiled one: a tree
// with children before their parents, whose nodes read only columns produced
// below them, each column produced once and with the type it is declared as,
// reading its dataframe arguments 0 to n - 1 once each.
class PlanChecker {
 public:
  explicit PlanChecker(const LogicalPlan& plan)
      : plan_(plan),
        produced_(plan.columns.size(), false),
        has_parent_(plan.nodes.size(), false),
        after_(plan.nodes.size()) {}

  bool Check() {
    if (plan_.nodes.empty() || plan_.root >= plan_.nodes.size()) {
      return false;
    }
    for (uint32_t i = 0; i < plan_.nodes.size(); ++i) {
      if (!CheckNode(i)) {
        return false;
      }
    }
    if (has_parent_[plan_.root]) {
      return false;
    }
    for (const NamedColumn& column : plan_.output) {
      if (!Has(plan_.root, column.id)) {
        return false;
      }
    }
    // Each dataframe argument is read by one scan, numbered from 0.
    std::sort(args_.begin(), args_.end());
    for (uint32_t i = 0; i < args_.size(); ++i) {
      if (args_[i] != i) {
        return false;
      }
    }
    return true;
  }

 private:
  bool CheckNode(uint32_t i) {
    const PlanNode& node = plan_.nodes[i];
    for (PlanNodeId child : node.children) {
      if (child >= i || has_parent_[child]) {
        return false;
      }
      has_parent_[child] = true;
    }
    after_[i].assign(plan_.columns.size(), false);
    switch (node.kind()) {
      case OpKind::GetTypeIndex<op::Scan>():
        return CheckScan(i);
      case OpKind::GetTypeIndex<op::TreeAccumulate>():
        return CheckTreeAccumulate(i);
      case OpKind::GetTypeIndex<op::IntervalIntersect>():
        return CheckIntervalIntersect(i);
      default:
        PERFETTO_FATAL("Unknown operator");
    }
  }

  bool CheckScan(uint32_t i) {
    const PlanNode& node = plan_.nodes[i];
    const auto& scan = node.Cast<op::Scan>();
    // A batch with no columns has no rows, so pruning always leaves one.
    if (!node.children.empty() || scan.columns.empty()) {
      return false;
    }
    // Dataframe columns are checked against the dataframe once it is found.
    if (const auto* arg = std::get_if<op::Scan::DataframeArg>(&scan.source)) {
      args_.push_back(arg->index);
    }
    for (const NamedColumn& column : scan.columns) {
      if (!Produce(i, column.id)) {
        return false;
      }
    }
    return true;
  }

  bool CheckTreeAccumulate(uint32_t i) {
    const PlanNode& node = plan_.nodes[i];
    if (node.children.size() != 1) {
      return false;
    }
    PlanNodeId in = node.children[0];
    const auto& acc = node.Cast<op::TreeAccumulate>();
    // Pruning removes a fold with nothing left to compute.
    if (acc.aggregates.empty() || !Has(in, acc.node_column) ||
        !Has(in, acc.parent_column)) {
      return false;
    }
    after_[i] = after_[in];
    for (const op::TreeAccumulate::Aggregate& agg : acc.aggregates) {
      if (!Has(in, agg.column) || !ProduceInt64(i, agg.output)) {
        return false;
      }
    }
    return true;
  }

  bool CheckIntervalIntersect(uint32_t i) {
    const PlanNode& node = plan_.nodes[i];
    const auto& isect = node.Cast<op::IntervalIntersect>();
    if (isect.operands.size() < 2 ||
        isect.operands.size() != node.children.size() ||
        !ProduceInt64(i, isect.ts) || !ProduceInt64(i, isect.dur)) {
      return false;
    }
    for (uint32_t k = 0; k < isect.operands.size(); ++k) {
      const op::IntervalIntersect::Operand& operand = isect.operands[k];
      PlanNodeId in = node.children[k];
      // Operands are compared key by key, and each runs as a scan of its own.
      if (!plan_.nodes[in].Is<op::Scan>() ||
          operand.keys.size() != isect.operands[0].keys.size() ||
          !Has(in, operand.ts) || !Has(in, operand.dur)) {
        return false;
      }
      for (ColumnId id : operand.keys) {
        if (!Has(in, id)) {
          return false;
        }
      }
      for (ColumnId id : operand.carried) {
        if (!Has(in, id)) {
          return false;
        }
        after_[i][id] = true;
      }
    }
    return true;
  }

  // Whether column `id` is available after node `node`.
  bool Has(PlanNodeId node, ColumnId id) const {
    return id < after_[node].size() && after_[node][id];
  }

  // Makes column `id` available after node `node`. The compiler gives every
  // column its own producer.
  bool Produce(PlanNodeId node, ColumnId id) {
    if (id >= produced_.size() || produced_[id]) {
      return false;
    }
    produced_[id] = true;
    after_[node][id] = true;
    return true;
  }

  // As Produce, for a column the executor always fills with Int64s.
  bool ProduceInt64(PlanNodeId node, ColumnId id) {
    return Produce(node, id) && Is<core::Int64>(plan_.columns[id].type);
  }

  template <typename T>
  static bool Is(const std::optional<core::StorageType>& type) {
    return type && type->Is<T>();
  }

  const LogicalPlan& plan_;
  std::vector<bool> produced_;
  std::vector<bool> has_parent_;
  std::vector<std::vector<bool>> after_;
  // The index of each dataframe argument a scan reads.
  std::vector<uint32_t> args_;
};

// The position in `dataframe` of the column a scan reads as `name`, if it has
// one.
std::optional<uint32_t> FindScanColumn(const dataframe::Dataframe& dataframe,
                                       const std::string& name) {
  const std::vector<std::string>& names = dataframe.column_names();
  for (uint32_t i = 0; i < names.size(); ++i) {
    if (names[i] == name && !dataframe::IsHiddenColumn(names[i])) {
      return i;
    }
  }
  return std::nullopt;
}

// Points each dataframe scan at the dataframe now registered under its name,
// which must still have every column the plan reads, with the same type.
base::Status ResolveDataframes(LogicalPlan& plan, const Catalog& catalog) {
  for (PlanNode& node : plan.nodes) {
    if (!node.Is<op::Scan>()) {
      continue;
    }
    auto& scan = node.Cast<op::Scan>();
    auto* source = std::get_if<op::Scan::Dataframe>(&scan.source);
    if (!source) {
      continue;
    }
    const dataframe::Dataframe* dataframe = catalog.FindDataframe(source->name);
    if (!dataframe) {
      return base::ErrStatus("Pipeline: table '%s' no longer exists",
                             source->name.c_str());
    }
    for (const NamedColumn& column : scan.columns) {
      std::optional<uint32_t> i = FindScanColumn(*dataframe, column.name);
      const std::optional<core::StorageType>& type =
          plan.columns[column.id].type;
      if (!i || !type || !(*type == dataframe->column_type(*i))) {
        return base::ErrStatus(
            "Pipeline: table '%s' has changed since the pipeline was written",
            source->name.c_str());
      }
      source->columns.push_back(dataframe->shared_column(*i));
    }
    source->row_count = dataframe->row_count();
  }
  return base::OkStatus();
}

}  // namespace

std::string SerializePlan(const LogicalPlan& plan) {
  Writer w;
  w.U32(kVersion);
  w.Size(plan.columns.size());
  for (const ColumnSchema& column : plan.columns) {
    w.Str(column.name);
    w.U8(column.type ? static_cast<uint8_t>(column.type->index() + 1) : 0);
  }
  w.Size(plan.nodes.size());
  for (const PlanNode& node : plan.nodes) {
    w.U8(static_cast<uint8_t>(node.kind()));
    w.Size(node.children.size());
    for (PlanNodeId child : node.children) {
      w.U32(child);
    }
    switch (node.kind()) {
      case OpKind::GetTypeIndex<op::Scan>():
        WriteScan(w, node.Cast<op::Scan>());
        break;
      case OpKind::GetTypeIndex<op::TreeAccumulate>():
        WriteTreeAccumulate(w, node.Cast<op::TreeAccumulate>());
        break;
      case OpKind::GetTypeIndex<op::IntervalIntersect>():
        WriteIntervalIntersect(w, node.Cast<op::IntervalIntersect>());
        break;
      default:
        PERFETTO_FATAL("Unknown operator");
    }
  }
  w.U32(plan.root);
  w.Size(plan.output.size());
  for (const NamedColumn& column : plan.output) {
    w.Str(column.name);
    w.U32(column.id);
  }
  return w.Take();
}

base::Status BindDataframeArgs(
    LogicalPlan& plan,
    const std::vector<const dataframe::Dataframe*>& args,
    StringPool* pool) {
  // Each argument is read by one scan, as the plan checker ensures.
  uint32_t count = 0;
  for (const PlanNode& node : plan.nodes) {
    if (node.Is<op::Scan>() && std::holds_alternative<op::Scan::DataframeArg>(
                                   node.Cast<op::Scan>().source)) {
      ++count;
    }
  }
  if (count != args.size()) {
    return base::ErrStatus("Pipeline: expected %u dataframe arguments, not %zu",
                           count, args.size());
  }
  for (PlanNode& node : plan.nodes) {
    if (!node.Is<op::Scan>()) {
      continue;
    }
    auto& scan = node.Cast<op::Scan>();
    const auto* arg = std::get_if<op::Scan::DataframeArg>(&scan.source);
    if (!arg) {
      continue;
    }
    // A relation with no rows is passed as null: read it as empty columns.
    std::optional<dataframe::Dataframe> empty;
    const dataframe::Dataframe* dataframe = args[arg->index];
    if (!dataframe) {
      std::vector<std::string> names;
      for (const NamedColumn& column : scan.columns) {
        names.push_back(column.name);
      }
      dataframe::AdhocDataframeBuilder::Options options;
      options.emit_auto_id = false;
      ASSIGN_OR_RETURN(empty, dataframe::AdhocDataframeBuilder(std::move(names),
                                                               pool, options)
                                  .Build());
      dataframe = &*empty;
    }
    op::Scan::Dataframe source;
    source.name = "dataframe argument " + std::to_string(arg->index);
    for (const NamedColumn& column : scan.columns) {
      std::optional<uint32_t> i = FindScanColumn(*dataframe, column.name);
      if (!i) {
        return base::ErrStatus("Pipeline: %s has no column '%s'",
                               source.name.c_str(), column.name.c_str());
      }
      // The dataframe was built after the plan was written, so it decides
      // what each column holds.
      plan.columns[column.id].type = dataframe->column_type(*i);
      source.columns.push_back(dataframe->shared_column(*i));
    }
    source.row_count = dataframe->row_count();
    scan.source = std::move(source);
  }
  return base::OkStatus();
}

base::StatusOr<LogicalPlan> DeserializePlan(std::string_view bytes,
                                            const Catalog& catalog) {
  Reader r(bytes);
  if (r.U32() != kVersion) {
    return Malformed();
  }
  LogicalPlan plan;
  plan.columns.resize(r.Count());
  for (ColumnSchema& column : plan.columns) {
    column.name = r.Str();
    column.type = ReadType(r);
  }
  plan.nodes.resize(r.Count());
  for (PlanNode& node : plan.nodes) {
    node = ReadNode(r);
  }
  plan.root = r.U32();
  plan.output.resize(r.Count());
  for (NamedColumn& column : plan.output) {
    column.name = r.Str();
    column.id = r.U32();
  }
  if (!r.done() || !PlanChecker(plan).Check()) {
    return Malformed();
  }
  RETURN_IF_ERROR(ResolveDataframes(plan, catalog));
  return std::move(plan);
}

}  // namespace perfetto::trace_processor::pipeline
