/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/plugins/pprof_functions/pprof_functions.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/basic_types.h"
#include "protos/perfetto/trace_processor/stack.pbzero.h"
#include "protos/third_party/pprof/profile.pbzero.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/profile_builder.h"

// TODO(carlscab): We currently recreate the GProfileBuilder for every group. We
// should cache this somewhere maybe even have a helper table that stores all
// this data.

namespace perfetto::trace_processor {
namespace {

using protos::pbzero::Stack;

template <typename T>
std::unique_ptr<T> WrapUnique(T* ptr) {
  return std::unique_ptr<T>(ptr);
}

class AggregateContext {
 public:
  static base::StatusOr<std::unique_ptr<AggregateContext>>
  Create(TraceProcessorContext* tp_context, size_t argc, sqlite3_value** argv) {
    base::StatusOr<std::vector<GProfileBuilder::ValueType>> sample_types =
        GetSampleTypes(argc, argv);
    if (!sample_types.ok()) {
      return sample_types.status();
    }
    return WrapUnique(new AggregateContext(tp_context, sample_types.value()));
  }

  base::Status Step(size_t argc, sqlite3_value** argv) {
    RETURN_IF_ERROR(UpdateSampleValue(argc, argv));

    base::StatusOr<SqlValue> value = sqlite::utils::ExtractArgument(
        argc, argv, "stack", 0, SqlValue::kBytes);
    if (!value.ok()) {
      return value.status();
    }

    Stack::Decoder stack(static_cast<const uint8_t*>(value->bytes_value),
                         value->bytes_count);
    if (stack.bytes_left() != 0) {
      return sqlite::utils::ToInvalidArgumentError(
          "stack", 0, base::ErrStatus("failed to deserialize Stack proto"));
    }
    if (!builder_.AddSample(stack, sample_values_)) {
      return base::ErrStatus("Failed to add callstack");
    }
    return base::OkStatus();
  }

  void Final(sqlite3_context* ctx) {
    std::string profile_proto = builder_.Build();
    return sqlite::result::TransientBytes(
        ctx, profile_proto.data(), static_cast<int>(profile_proto.size()));
  }

 private:
  static base::StatusOr<std::vector<GProfileBuilder::ValueType>> GetSampleTypes(
      size_t argc,
      sqlite3_value** argv) {
    std::vector<GProfileBuilder::ValueType> sample_types;

    if (argc == 1) {
      sample_types.push_back({"samples", "count"});
    }

    for (size_t i = 1; i < argc; i += 3) {
      base::StatusOr<SqlValue> type = sqlite::utils::ExtractArgument(
          argc, argv, "sample_type", i, SqlValue::kString);
      if (!type.ok()) {
        return type.status();
      }

      base::StatusOr<SqlValue> units = sqlite::utils::ExtractArgument(
          argc, argv, "sample_units", i + 1, SqlValue::kString);
      if (!units.ok()) {
        return units.status();
      }

      sample_types.push_back({type->AsString(), units->AsString()});
    }
    return sample_types;
  }

  AggregateContext(TraceProcessorContext* tp_context,
                   const std::vector<GProfileBuilder::ValueType>& sample_types)
      : builder_(tp_context, sample_types) {
    sample_values_.resize(sample_types.size(), 1);
  }

  base::Status UpdateSampleValue(size_t argc, sqlite3_value** argv) {
    if (argc == 1) {
      PERFETTO_CHECK(sample_values_.size() == 1);
      return base::OkStatus();
    }

    PERFETTO_CHECK(argc == 1 + (sample_values_.size() * 3));
    for (size_t i = 0; i < sample_values_.size(); ++i) {
      base::StatusOr<SqlValue> value = sqlite::utils::ExtractArgument(
          argc, argv, "sample_value", 3 + i * 3, SqlValue::kLong);
      if (!value.ok()) {
        return value.status();
      }
      sample_values_[i] = value->AsLong();
    }

    return base::OkStatus();
  }

  GProfileBuilder builder_;
  std::vector<int64_t> sample_values_;
};

base::Status StepStatus(sqlite3_context* ctx,
                        size_t argc,
                        sqlite3_value** argv) {
  auto** agg_context_ptr = static_cast<AggregateContext**>(
      sqlite3_aggregate_context(ctx, sizeof(AggregateContext*)));
  if (!agg_context_ptr) {
    return base::ErrStatus("Failed to allocate aggregate context");
  }

  if (!*agg_context_ptr) {
    auto* tp_context =
        static_cast<TraceProcessorContext*>(sqlite3_user_data(ctx));
    base::StatusOr<std::unique_ptr<AggregateContext>> agg_context =
        AggregateContext::Create(tp_context, argc, argv);
    if (!agg_context.ok()) {
      return agg_context.status();
    }

    *agg_context_ptr = agg_context->release();
  }

  return (*agg_context_ptr)->Step(argc, argv);
}

struct ProfileBuilder {
  static constexpr char kName[] = "EXPERIMENTAL_PROFILE";
  static constexpr int kArgCount = -1;
  using UserData = TraceProcessorContext;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_CHECK(argc >= 0);

    base::Status status = StepStatus(ctx, static_cast<size_t>(argc), argv);
    if (!status.ok()) {
      sqlite::utils::SetError(ctx, kName, status);
    }
  }

  static void Final(sqlite3_context* ctx) {
    auto** agg_context_ptr =
        static_cast<AggregateContext**>(sqlite3_aggregate_context(ctx, 0));

    if (!agg_context_ptr) {
      return;
    }

    (*agg_context_ptr)->Final(ctx);

    delete (*agg_context_ptr);
  }
};

// Aggregate that converts an `(id, parent_id, frame_name, self_value)` tree
// — together with constant `sample_type` and `unit` strings — into a
// serialized pprof Profile proto.
//
// Conventions:
// - `id` must be unique per row (duplicates fail the aggregate).
// - `parent_id` NULL marks a root; multiple roots are allowed.
// - `parent_id` referencing an unknown id fails the aggregate.
// - `self_value` <= 0 (or NULL) means no Sample is emitted for that node
//   but the location is still available as an ancestor for samples below.
// - `sample_type` and `unit` are read from the first row only; SQL callers
//   are expected to pass them as constants. Later rows are not re-checked.
//
// Order independence: rows are buffered during Step and resolved in Final,
// so any SQL ordering is correct.
class TreeAggregateContext {
 public:
  base::Status Step(size_t argc, sqlite3_value** argv) {
    if (argc != 6) {
      return base::ErrStatus(
          "PROFILE_FROM_TREE: expected 6 args (id, parent_id, frame_name, "
          "self_value, sample_type, unit); got %zu",
          argc);
    }

    base::StatusOr<SqlValue> id =
        sqlite::utils::ExtractArgument(argc, argv, "id", 0, SqlValue::kLong);
    if (!id.ok()) {
      return id.status();
    }

    Node node;
    node.id = id->AsLong();

    if (sqlite3_value_type(argv[1]) != SQLITE_NULL) {
      base::StatusOr<SqlValue> parent_id = sqlite::utils::ExtractArgument(
          argc, argv, "parent_id", 1, SqlValue::kLong);
      if (!parent_id.ok()) {
        return parent_id.status();
      }
      node.parent_id = parent_id->AsLong();
    }

    if (sqlite3_value_type(argv[2]) != SQLITE_NULL) {
      base::StatusOr<SqlValue> name = sqlite::utils::ExtractArgument(
          argc, argv, "frame_name", 2, SqlValue::kString);
      if (!name.ok()) {
        return name.status();
      }
      node.name = name->AsString();
    }

    if (sqlite3_value_type(argv[3]) != SQLITE_NULL) {
      base::StatusOr<SqlValue> value = sqlite::utils::ExtractArgument(
          argc, argv, "self_value", 3, SqlValue::kLong);
      if (!value.ok()) {
        return value.status();
      }
      node.self_value = value->AsLong();
    }

    if (sample_type_.empty()) {
      base::StatusOr<SqlValue> stype = sqlite::utils::ExtractArgument(
          argc, argv, "sample_type", 4, SqlValue::kString);
      if (!stype.ok()) {
        return stype.status();
      }
      sample_type_ = stype->AsString();

      base::StatusOr<SqlValue> u = sqlite::utils::ExtractArgument(
          argc, argv, "unit", 5, SqlValue::kString);
      if (!u.ok()) {
        return u.status();
      }
      unit_ = u->AsString();
    }

    auto [_, inserted] = id_to_index_.emplace(node.id, nodes_.size());
    if (!inserted) {
      return base::ErrStatus("PROFILE_FROM_TREE: duplicate id %" PRId64,
                             node.id);
    }
    nodes_.push_back(std::move(node));
    return base::OkStatus();
  }

  void Final(sqlite3_context* ctx) {
    base::Status status = Build(ctx);
    if (!status.ok()) {
      sqlite::utils::SetError(ctx, "PROFILE_FROM_TREE", status);
    }
  }

 private:
  struct Node {
    int64_t id = 0;
    std::optional<int64_t> parent_id;
    std::string name;
    int64_t self_value = 0;
  };

  // Adds `s` to the staged string_table if not already present. Indices
  // are 0-based; index 0 is always "" per the pprof format.
  int64_t InternString(const std::string& s) {
    auto it = string_index_.find(s);
    if (it != string_index_.end()) {
      return it->second;
    }
    auto index = static_cast<int64_t>(string_table_.size());
    string_table_.push_back(s);
    string_index_[s] = index;
    return index;
  }

  base::Status Build(sqlite3_context* ctx) {
    protozero::HeapBuffered<third_party::perftools::profiles::pbzero::Profile>
        profile;

    // protozero only allows one open child submessage at a time. We
    // therefore stage every string in `string_table_` first, so writing
    // a submessage never needs to insert a new top-level string_table
    // field while the child is still open.
    InternString("");

    if (sample_type_.empty()) {
      // No rows. Emit a valid, empty Profile (just the empty string).
      profile->add_string_table(string_table_[0]);
      std::string out = profile.SerializeAsString();
      sqlite::result::TransientBytes(ctx, out.data(),
                                     static_cast<int>(out.size()));
      return base::OkStatus();
    }

    int64_t type_idx = InternString(sample_type_);
    int64_t unit_idx = InternString(unit_);

    // Stage one Function per unique frame_name and remember the
    // assigned function id keyed by name. Function ids start at 1.
    std::unordered_map<std::string, uint64_t> name_to_function_id;
    struct StagedFunction {
      uint64_t id;
      int64_t name_idx;
    };
    std::vector<StagedFunction> staged_functions;
    auto get_function_id = [&](const std::string& name) -> uint64_t {
      auto it = name_to_function_id.find(name);
      if (it != name_to_function_id.end()) {
        return it->second;
      }
      uint64_t id = name_to_function_id.size() + 1;
      name_to_function_id[name] = id;
      staged_functions.push_back({id, InternString(name)});
      return id;
    };

    // Location id == nodes_index + 1 (dense, stable).
    std::vector<uint64_t> location_function_id(nodes_.size());
    for (size_t i = 0; i < nodes_.size(); ++i) {
      location_function_id[i] = get_function_id(nodes_[i].name);
    }

    // Validate the parent chain: every non-NULL parent_id must point at
    // a known id. Cycle detection is deferred to the sample walk where
    // it is per-sample.
    std::vector<std::optional<size_t>> parent_index(nodes_.size());
    for (size_t i = 0; i < nodes_.size(); ++i) {
      const auto& n = nodes_[i];
      if (!n.parent_id) {
        continue;
      }
      auto it = id_to_index_.find(*n.parent_id);
      if (it == id_to_index_.end()) {
        return base::ErrStatus("PROFILE_FROM_TREE: id %" PRId64
                               " has parent_id %" PRId64
                               " which was not seen in the input",
                               n.id, *n.parent_id);
      }
      parent_index[i] = it->second;
    }

    {
      auto* st = profile->add_sample_type();
      st->set_type(type_idx);
      st->set_unit(unit_idx);
    }

    for (const auto& fn : staged_functions) {
      auto* f = profile->add_function();
      f->set_id(fn.id);
      f->set_name(fn.name_idx);
      f->set_system_name(fn.name_idx);
    }

    for (size_t i = 0; i < nodes_.size(); ++i) {
      auto* loc = profile->add_location();
      loc->set_id(static_cast<uint64_t>(i + 1));
      auto* line = loc->add_line();
      line->set_function_id(location_function_id[i]);
    }

    // For every node with a positive self_value emit one Sample whose
    // location stack is the path from the node up to the root. pprof's
    // Sample.location_id and Sample.value are packed-varint repeated
    // fields; pbzero exposes them via PackedVarInt + set_*.
    for (size_t i = 0; i < nodes_.size(); ++i) {
      const auto& n = nodes_[i];
      if (n.self_value <= 0) {
        continue;
      }
      protozero::PackedVarInt locs;
      std::unordered_set<size_t> visited;
      for (std::optional<size_t> cur = i; cur; cur = parent_index[*cur]) {
        if (!visited.insert(*cur).second) {
          return base::ErrStatus(
              "PROFILE_FROM_TREE: cycle detected at id %" PRId64,
              nodes_[*cur].id);
        }
        locs.Append(static_cast<uint64_t>(*cur + 1));
      }
      protozero::PackedVarInt vals;
      vals.Append(n.self_value);

      auto* sample = profile->add_sample();
      sample->set_location_id(locs);
      sample->set_value(vals);
    }

    for (const auto& s : string_table_) {
      profile->add_string_table(s);
    }

    std::string out = profile.SerializeAsString();
    sqlite::result::TransientBytes(ctx, out.data(),
                                   static_cast<int>(out.size()));
    return base::OkStatus();
  }

  std::vector<Node> nodes_;
  std::unordered_map<int64_t, size_t> id_to_index_;
  std::vector<std::string> string_table_;
  std::unordered_map<std::string, int64_t> string_index_;
  std::string sample_type_;
  std::string unit_;
};

base::Status TreeStepStatus(sqlite3_context* ctx,
                            size_t argc,
                            sqlite3_value** argv) {
  auto** agg_context_ptr = static_cast<TreeAggregateContext**>(
      sqlite3_aggregate_context(ctx, sizeof(TreeAggregateContext*)));
  if (!agg_context_ptr) {
    return base::ErrStatus("Failed to allocate aggregate context");
  }
  if (!*agg_context_ptr) {
    *agg_context_ptr = new TreeAggregateContext();
  }
  return (*agg_context_ptr)->Step(argc, argv);
}

struct ProfileFromTree {
  static constexpr char kName[] = "PROFILE_FROM_TREE";
  static constexpr int kArgCount = 6;
  using UserData = TraceProcessorContext;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_CHECK(argc >= 0);
    base::Status status = TreeStepStatus(ctx, static_cast<size_t>(argc), argv);
    if (!status.ok()) {
      sqlite::utils::SetError(ctx, kName, status);
    }
  }

  static void Final(sqlite3_context* ctx) {
    auto** agg_context_ptr =
        static_cast<TreeAggregateContext**>(sqlite3_aggregate_context(ctx, 0));
    if (!agg_context_ptr) {
      return;
    }
    (*agg_context_ptr)->Final(ctx);
    delete (*agg_context_ptr);
  }
};

}  // namespace

namespace pprof_functions {
namespace {

class PprofFunctionsPlugin : public Plugin<PprofFunctionsPlugin> {
 public:
  ~PprofFunctionsPlugin() override;

  void RegisterAggregateFunctions(
      PerfettoSqlConnection*,
      std::vector<AggregateFunctionRegistration>& out) override {
    out.push_back(MakeAggregateRegistration<ProfileBuilder>(trace_context_));
    out.push_back(MakeAggregateRegistration<ProfileFromTree>(trace_context_));
  }
};

PprofFunctionsPlugin::~PprofFunctionsPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<PprofFunctionsPlugin>();
      },
      PprofFunctionsPlugin::kPluginId, PprofFunctionsPlugin::kDepIds.data(),
      PprofFunctionsPlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace pprof_functions

}  // namespace perfetto::trace_processor
