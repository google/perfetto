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

// A plan is written as its source then the stages above it, naming columns by
// position among those available, so only plans the compiler could build can
// be written and reading one needs only bounds checks.

class Writer {
 public:
  void U8(uint8_t value) { Append(&value, sizeof(value)); }
  void U32(uint32_t value) { Append(&value, sizeof(value)); }
  void Size(size_t value) { U32(static_cast<uint32_t>(value)); }
  void Str(std::string_view value) {
    Size(value.size());
    out_.append(value.data(), value.size());
  }
  // Out of bounds if `id` is not available.
  void Position(const std::vector<ColumnId>& available, ColumnId id) {
    auto it = std::find(available.begin(), available.end(), id);
    Size(static_cast<size_t>(it - available.begin()));
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

// After the first failure, every read returns zeroes.
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
  // At most the bytes left, so a corrupt count cannot allocate much.
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
  ColumnId Position(const std::vector<ColumnId>& available) {
    uint32_t i = U32();
    if (i >= available.size()) {
      Fail();
      return 0;
    }
    return available[i];
  }

  void Fail() { ok_ = false; }
  bool ok() const { return ok_; }
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

void WriteType(Writer& w, const std::optional<core::StorageType>& type) {
  // Zero is a type known only per row; otherwise the type's index plus one.
  w.U8(type ? static_cast<uint8_t>(type->index() + 1) : 0);
}

std::optional<core::StorageType> ReadType(Reader& r) {
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

using Available = std::vector<ColumnId>;

class PlanWriter {
 public:
  explicit PlanWriter(const LogicalPlan& plan) : plan_(plan) {}

  std::string Write() {
    // Pruning can leave nodes the root does not reach.
    std::vector<const op::TreeAccumulate*> folds;
    PlanNodeId id = plan_.root;
    while (plan_.nodes[id].Is<op::TreeAccumulate>()) {
      folds.push_back(&plan_.nodes[id].Cast<op::TreeAccumulate>());
      id = plan_.nodes[id].children[0];
    }
    const PlanNode& source = plan_.nodes[id];
    w_.U8(static_cast<uint8_t>(source.op.index()));
    Available available = source.Is<op::Scan>()
                              ? WriteScan(source.Cast<op::Scan>())
                              : WriteIntervalIntersect(source);
    w_.Size(folds.size());
    for (auto it = folds.rbegin(); it != folds.rend(); ++it) {
      WriteTreeAccumulate(**it, available);
    }
    w_.Size(plan_.output.size());
    for (const NamedColumn& column : plan_.output) {
      w_.Str(column.name);
      w_.Position(available, column.id);
    }
    return w_.Take();
  }

 private:
  Available WriteScan(const op::Scan& scan) {
    w_.U8(static_cast<uint8_t>(scan.source.index()));
    switch (scan.source.index()) {
      case base::variant_index<op::Scan::Source, op::Scan::Dataframe>():
        w_.Str(base::unchecked_get<op::Scan::Dataframe>(scan.source).name);
        break;
      case base::variant_index<op::Scan::Source, op::Scan::DataframeArg>():
        w_.U32(base::unchecked_get<op::Scan::DataframeArg>(scan.source).index);
        break;
      default:
        // SQL is moved out into dataframe arguments before a plan is written.
        PERFETTO_FATAL("Unknown scan source");
    }
    w_.Size(scan.columns.size());
    Available available;
    for (const NamedColumn& column : scan.columns) {
      w_.Str(column.name);
      WriteType(w_, plan_.columns[column.id].type);
      available.push_back(column.id);
    }
    return available;
  }

  void WriteTreeAccumulate(const op::TreeAccumulate& acc,
                           Available& available) {
    w_.U8(static_cast<uint8_t>(acc.direction));
    w_.Position(available, acc.node_column);
    w_.Position(available, acc.parent_column);
    w_.Size(acc.aggregates.size());
    Available outputs;
    for (const op::TreeAccumulate::Aggregate& agg : acc.aggregates) {
      w_.Position(available, agg.column);
      w_.Str(plan_.columns[agg.output].name);
      outputs.push_back(agg.output);
    }
    available.insert(available.end(), outputs.begin(), outputs.end());
  }

  Available WriteIntervalIntersect(const PlanNode& node) {
    const auto& isect = node.Cast<op::IntervalIntersect>();
    w_.Str(plan_.columns[isect.ts].name);
    w_.Str(plan_.columns[isect.dur].name);
    w_.Size(isect.operands.size());
    w_.Size(isect.operands.empty() ? 0 : isect.operands[0].keys.size());
    Available available{isect.ts, isect.dur};
    for (uint32_t k = 0; k < isect.operands.size(); ++k) {
      const op::IntervalIntersect::Operand& operand = isect.operands[k];
      PERFETTO_CHECK(plan_.nodes[node.children[k]].Is<op::Scan>());
      Available in = WriteScan(plan_.nodes[node.children[k]].Cast<op::Scan>());
      w_.Position(in, operand.ts);
      w_.Position(in, operand.dur);
      for (ColumnId id : operand.keys) {
        w_.Position(in, id);
      }
      w_.Size(operand.carried.size());
      for (ColumnId id : operand.carried) {
        w_.Position(in, id);
        available.push_back(id);
      }
    }
    return available;
  }

  const LogicalPlan& plan_;
  Writer w_;
};

class PlanReader {
 public:
  explicit PlanReader(Reader& r) : r_(r) {}

  LogicalPlan Read() {
    Available available;
    switch (r_.U8()) {
      case base::variant_index<Op, op::Scan>(): {
        op::Scan scan;
        available = ReadScan(scan);
        plan_.AddNode(std::move(scan));
        break;
      }
      case base::variant_index<Op, op::IntervalIntersect>():
        ReadIntervalIntersect(available);
        break;
      default:
        r_.Fail();
        return {};
    }
    uint32_t folds = r_.Count();
    for (uint32_t i = 0; i < folds && r_.ok(); ++i) {
      plan_.AddNode(ReadTreeAccumulate(available), {plan_.root});
    }
    plan_.output.resize(r_.Count());
    for (NamedColumn& column : plan_.output) {
      column.name = r_.Str();
      column.id = r_.Position(available);
    }
    return std::move(plan_);
  }

 private:
  Available ReadScan(op::Scan& scan) {
    switch (r_.U8()) {
      case base::variant_index<op::Scan::Source, op::Scan::Dataframe>(): {
        op::Scan::Dataframe source;
        source.name = r_.Str();
        scan.source = std::move(source);
        break;
      }
      case base::variant_index<op::Scan::Source, op::Scan::DataframeArg>():
        scan.source = op::Scan::DataframeArg{r_.U32()};
        break;
      default:
        r_.Fail();
        return {};
    }
    scan.columns.resize(r_.Count());
    Available available;
    for (NamedColumn& column : scan.columns) {
      column.name = r_.Str();
      column.id = plan_.AddColumn(column.name, ReadType(r_));
      available.push_back(column.id);
    }
    return available;
  }

  op::TreeAccumulate ReadTreeAccumulate(Available& available) {
    op::TreeAccumulate acc;
    acc.direction = static_cast<op::TreeDirection>(r_.U8());
    acc.node_column = r_.Position(available);
    acc.parent_column = r_.Position(available);
    acc.aggregates.resize(r_.Count());
    for (op::TreeAccumulate::Aggregate& agg : acc.aggregates) {
      agg.column = r_.Position(available);
      agg.output = plan_.AddColumn(r_.Str(), core::Int64{});
    }
    for (const op::TreeAccumulate::Aggregate& agg : acc.aggregates) {
      available.push_back(agg.output);
    }
    return acc;
  }

  void ReadIntervalIntersect(Available& available) {
    op::IntervalIntersect isect;
    isect.ts = plan_.AddColumn(r_.Str(), core::Int64{});
    isect.dur = plan_.AddColumn(r_.Str(), core::Int64{});
    available = {isect.ts, isect.dur};
    isect.operands.resize(r_.Count());
    uint32_t keys = r_.Count();
    if (isect.operands.size() < 2) {
      r_.Fail();
    }
    std::vector<PlanNodeId> children;
    for (op::IntervalIntersect::Operand& operand : isect.operands) {
      op::Scan scan;
      Available in = ReadScan(scan);
      if (!r_.ok()) {
        return;
      }
      children.push_back(plan_.AddNode(std::move(scan)));
      operand.ts = r_.Position(in);
      operand.dur = r_.Position(in);
      operand.keys.resize(keys);
      for (ColumnId& id : operand.keys) {
        id = r_.Position(in);
      }
      operand.carried.resize(r_.Count());
      for (ColumnId& id : operand.carried) {
        id = r_.Position(in);
        available.push_back(id);
      }
    }
    plan_.AddNode(std::move(isect), std::move(children));
  }

  Reader& r_;
  LogicalPlan plan_;
};

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
  return PlanWriter(plan).Write();
}

base::Status BindDataframeArgs(
    LogicalPlan& plan,
    const std::vector<const dataframe::Dataframe*>& args,
    StringPool* pool) {
  for (PlanNode& node : plan.nodes) {
    if (!node.Is<op::Scan>()) {
      continue;
    }
    auto& scan = node.Cast<op::Scan>();
    const auto* arg = std::get_if<op::Scan::DataframeArg>(&scan.source);
    if (!arg) {
      continue;
    }
    if (arg->index >= args.size()) {
      return base::ErrStatus("Pipeline: no dataframe argument %u", arg->index);
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
  LogicalPlan plan = PlanReader(r).Read();
  if (!r.done()) {
    return base::ErrStatus("__intrinsic_pipeline: malformed plan");
  }
  RETURN_IF_ERROR(ResolveDataframes(plan, catalog));
  return std::move(plan);
}

}  // namespace perfetto::trace_processor::pipeline
