/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/plugins/winscope_proto_to_args_with_defaults/winscope_proto_to_args_with_defaults.h"

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
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
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/cursor.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/importers/common/arg_sink.h"
#include "src/trace_processor/importers/proto/proto_to_args_parser.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/plugins/winscope_importer/deinterning_arg_sink.h"
#include "src/trace_processor/plugins/winscope_importer/winscope_importer.h"
#include "src/trace_processor/plugins/winscope_importer/winscope_proto_mapping.h"
#include "src/trace_processor/plugins/winscope_proto_to_args_with_defaults/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto::trace_processor::winscope_proto_to_args_with_defaults {

namespace {
class WinscopeProtoToArgsWithDefaults : public StaticTableFunction {
 public:
  class Cursor : public StaticTableFunction::Cursor {
   public:
    explicit Cursor(StringPool* string_pool,
                    const PerfettoSqlConnection* connection,
                    TraceProcessorContext* context);
    bool Run(const std::vector<SqlValue>& arguments) override;

   private:
    StringPool* string_pool_ = nullptr;
    const PerfettoSqlConnection* engine_ = nullptr;
    TraceProcessorContext* context_ = nullptr;
    tables::WinscopeArgsWithDefaultsTable table_;
  };

  explicit WinscopeProtoToArgsWithDefaults(StringPool*,
                                           const PerfettoSqlConnection*,
                                           TraceProcessorContext* context);

  std::unique_ptr<StaticTableFunction::Cursor> MakeCursor() override;
  dataframe::DataframeSpec CreateSpec() override;
  std::string TableName() override;
  uint32_t GetArgumentCount() const override;

 private:
  StringPool* string_pool_ = nullptr;
  const PerfettoSqlConnection* engine_ = nullptr;
  TraceProcessorContext* context_ = nullptr;
};

// Helper to extract uint32 values via Cell() callback.
struct Uint32CellExtractor : dataframe::CellCallback {
  std::optional<uint32_t> result;
  void OnCell(int64_t) {}
  void OnCell(double) {}
  void OnCell(NullTermStringView) {}
  void OnCell(std::nullptr_t) { result = std::nullopt; }
  void OnCell(uint32_t v) { result = v; }
  void OnCell(int32_t) {}
};

// Helper to extract int64 values via Cell() callback.
struct Int64CellExtractor : dataframe::CellCallback {
  int64_t result = 0;
  void OnCell(int64_t v) { result = v; }
  void OnCell(double) {}
  void OnCell(NullTermStringView) {}
  void OnCell(std::nullptr_t) {}
  void OnCell(uint32_t) {}
  void OnCell(int32_t) {}
};

// Interned data stored in table with columns:
// - base64_proto_id
// - flat_key
// - iid
// - deinterned_value
// Mapping reconstructed using nested FlatHashMaps to optionally
// deintern strings from proto data.
using ProtoId = uint32_t;
using FlatKey = StringPool::Id;
using Iid = int64_t;
using DeinternedValue = StringPool::Id;

using DeinternedIids = base::FlatHashMap<Iid, DeinternedValue>;
using InternedData = base::FlatHashMap<FlatKey, DeinternedIids>;
using ProtoToInternedData = base::FlatHashMap<ProtoId, InternedData>;

ProtoToInternedData GetProtoToInternedData(const std::string& table_name,
                                           TraceStorage* storage) {
  ProtoToInternedData proto_to_interned_data;
  const auto* interned_data_table =
      util::winscope_proto_mapping::GetInternedDataTable(table_name, storage);
  if (interned_data_table) {
    for (auto it = interned_data_table->IterateRows(); it; ++it) {
      const auto proto_id = it.base64_proto_id();
      const auto flat_key = it.flat_key();
      const auto iid = it.iid();
      const auto deinterned_value = it.deinterned_value();
      auto& deinterned_iids = proto_to_interned_data[proto_id][flat_key];
      deinterned_iids.Insert(iid, deinterned_value);
    }
  }
  return proto_to_interned_data;
}

using RowReference = tables::WinscopeArgsWithDefaultsTable::RowReference;
using Row = tables::WinscopeArgsWithDefaultsTable::Row;
using RowId = tables::WinscopeArgsWithDefaultsTable::Id;
using KeyToRowMap = std::unordered_map<StringPool::Id, RowId>;

// Writes each arg as a row of the args-with-defaults table. With a group's
// |key_to_row| map, args of the same key across the group share one row and
// defaults are only added for keys the group has not seen.
class WinscopeArgsSink : public ArgSink {
 public:
  WinscopeArgsSink(const uint32_t base64_proto_id,
                   tables::WinscopeArgsWithDefaultsTable* table,
                   KeyToRowMap* key_to_row)
      : base64_proto_id_(base64_proto_id),
        table_(table),
        key_to_row_(key_to_row) {}

  void Add(ArgKey key, Variadic value) override {
    RowReference r = GetOrCreateRow(key);
    switch (value.type) {
      case Variadic::kInt:
        r.set_int_value(value.int_value);
        break;
      case Variadic::kUint:
        r.set_int_value(static_cast<int64_t>(value.uint_value));
        break;
      case Variadic::kBool:
        r.set_int_value(value.bool_value);
        break;
      case Variadic::kString:
        r.set_string_value(value.string_value);
        break;
      case Variadic::kReal:
        r.set_real_value(value.real_value);
        break;
      case Variadic::kNull:
        break;
      case Variadic::kPointer:
      case Variadic::kJson:
        PERFETTO_FATAL("Unsupported");
    }
  }

  bool WantsDefault(ArgKey key) override {
    return !key_to_row_ || key_to_row_->find(key.key) == key_to_row_->end();
  }

 private:
  RowReference GetOrCreateRow(ArgKey key) {
    RowId row_id;
    if (!key_to_row_) {
      row_id = table_->Insert(Row()).id;
    } else {
      auto pos = key_to_row_->find(key.key);
      if (pos != key_to_row_->end()) {
        row_id = pos->second;
      } else {
        row_id = table_->Insert(Row()).id;
        key_to_row_->insert({key.key, row_id});
      }
    }

    auto row = (*table_)[row_id];
    row.set_key(key.key);
    row.set_flat_key(key.flat_key);
    row.set_base64_proto_id(base64_proto_id_);
    return row;
  }

  const uint32_t base64_proto_id_;
  tables::WinscopeArgsWithDefaultsTable* table_;
  KeyToRowMap* key_to_row_;
};

base::Status InsertRows(
    const dataframe::Dataframe& static_table,
    tables::WinscopeArgsWithDefaultsTable* inflated_args_table,
    const std::string& proto_name,
    const std::vector<uint32_t>* allowed_fields,
    const std::string* group_id_col_name,
    DescriptorPool& descriptor_pool,
    StringPool* string_pool,
    const ProtoToInternedData& proto_to_interned_data,
    const std::string& table_name) {
  util::ProtoToArgsParser args_parser{descriptor_pool, *string_pool};

  auto it = static_table.IndexOfColumnLegacy("base64_proto_id");
  if (!it) {
    return base::ErrStatus("Table does not have a base64_proto_id column.");
  }
  uint32_t base64_col = *it;
  std::optional<uint32_t> group_id_col_idx;
  if (group_id_col_name) {
    group_id_col_idx = static_table.IndexOfColumnLegacy(*group_id_col_name);
  }

  std::unordered_set<uint32_t> inflated_protos;
  std::unordered_map<uint32_t, KeyToRowMap> group_id_to_key_row_map;
  for (uint32_t i = 0; i < static_table.row_count(); ++i) {
    Uint32CellExtractor base64_extractor;
    static_table.GetCell(i, base64_col, base64_extractor);
    std::optional<uint32_t> base64_proto_id = base64_extractor.result;
    PERFETTO_CHECK(base64_proto_id.has_value());
    if (inflated_protos.count(*base64_proto_id) > 0) {
      continue;
    }
    inflated_protos.insert(*base64_proto_id);

    if (util::winscope_proto_mapping::ShouldSkipRow(table_name, static_table, i,
                                                    string_pool)) {
      continue;
    }

    const auto raw_proto =
        string_pool->Get(StringPool::Id::Raw(*base64_proto_id));
    const auto blob = *base::Base64Decode(raw_proto);
    const auto cb = protozero::ConstBytes{
        reinterpret_cast<const uint8_t*>(blob.data()), blob.size()};

    KeyToRowMap* key_to_row = nullptr;
    if (group_id_col_idx.has_value()) {
      Int64CellExtractor group_id_extractor;
      static_table.GetCell(i, *group_id_col_idx, group_id_extractor);
      uint32_t group_id = static_cast<uint32_t>(group_id_extractor.result);
      auto pos = group_id_to_key_row_map.find(group_id);
      if (pos != group_id_to_key_row_map.end()) {
        key_to_row = &(pos->second);
      } else {
        key_to_row = &(group_id_to_key_row_map[group_id]);
      }
    }
    InternedData* interned_data = proto_to_interned_data.Find(*base64_proto_id);
    WinscopeArgsSink args_sink(*base64_proto_id, inflated_args_table,
                               key_to_row);
    winscope::DeinterningArgSink deinterning_sink(
        args_sink, *string_pool,
        [interned_data, string_pool](
            ArgKey key, uint64_t iid) -> std::optional<base::StringView> {
          DeinternedIids* iids = interned_data->Find(key.flat_key);
          if (!iids) {
            return std::nullopt;
          }
          DeinternedValue* value = iids->Find(static_cast<Iid>(iid));
          if (!value) {
            return std::nullopt;
          }
          return string_pool->Get(*value);
        });
    ArgSink& sink = interned_data ? static_cast<ArgSink&>(deinterning_sink)
                                  : static_cast<ArgSink&>(args_sink);
    util::ProtoArgsEnv env;
    env.bytes = util::ProtoArgsEnv::BytesMode::kRaw;

    const std::vector<uint32_t>* allowed_fields_per_row = nullptr;
    std::optional<std::vector<uint32_t>> fields_per_row;
    if (!allowed_fields) {
      fields_per_row = util::winscope_proto_mapping::GetAllowedFieldsPerRow(
          table_name, static_table, i, string_pool);
      allowed_fields_per_row =
          fields_per_row ? &fields_per_row.value() : nullptr;
    }

    RETURN_IF_ERROR(args_parser.ParseMessage(
        cb, proto_name,
        allowed_fields ? allowed_fields : allowed_fields_per_row, env, sink,
        nullptr, true));
  }
  return base::OkStatus();
}

WinscopeProtoToArgsWithDefaults::Cursor::Cursor(
    StringPool* string_pool,
    const PerfettoSqlConnection* connection,
    TraceProcessorContext* context)
    : string_pool_(string_pool),
      engine_(connection),
      context_(context),
      table_(string_pool) {}

bool WinscopeProtoToArgsWithDefaults::Cursor::Run(
    const std::vector<SqlValue>& arguments) {
  PERFETTO_DCHECK(arguments.size() == 1);
  if (arguments[0].type != SqlValue::kString) {
    return OnFailure(base::ErrStatus(
        "__intrinsic_winscope_proto_to_args_with_defaults takes table name as "
        "a string."));
  }
  std::string table_name_str = arguments[0].AsString();
  // Table names now use the __intrinsic_ prefix internally. If the caller
  // passes the user-facing name, prepend the prefix.
  if (table_name_str.substr(0, 12) != "__intrinsic_") {
    table_name_str = "__intrinsic_" + table_name_str;
  }
  const dataframe::Dataframe* static_table_from_connection =
      engine_->GetDataframeOrNull(table_name_str);
  if (!static_table_from_connection) {
    return OnFailure(
        base::ErrStatus("Failed to find %s table.", table_name_str.c_str()));
  }

  base::StatusOr<const char*> proto_name =
      util::winscope_proto_mapping::GetProtoName(table_name_str);
  if (!proto_name.ok()) {
    return OnFailure(proto_name.status());
  }
  table_.Clear();

  auto allowed_fields =
      util::winscope_proto_mapping::GetAllowedFields(table_name_str);

  auto group_id_col_name =
      util::winscope_proto_mapping::GetGroupIdColName(table_name_str);
  auto proto_to_interned_data =
      GetProtoToInternedData(table_name_str, context_->storage.get());

  base::Status insert_status =
      InsertRows(*static_table_from_connection, &table_, *proto_name,
                 allowed_fields ? &allowed_fields.value() : nullptr,
                 group_id_col_name ? &group_id_col_name.value() : nullptr,
                 *context_->descriptor_pool_, string_pool_,
                 proto_to_interned_data, table_name_str);
  if (!insert_status.ok()) {
    return OnFailure(insert_status);
  }
  return OnSuccess(&table_.dataframe());
}

WinscopeProtoToArgsWithDefaults::WinscopeProtoToArgsWithDefaults(
    StringPool* string_pool,
    const PerfettoSqlConnection* connection,
    TraceProcessorContext* context)
    : string_pool_(string_pool), engine_(connection), context_(context) {}

std::unique_ptr<StaticTableFunction::Cursor>
WinscopeProtoToArgsWithDefaults::MakeCursor() {
  return std::make_unique<Cursor>(string_pool_, engine_, context_);
}

dataframe::DataframeSpec WinscopeProtoToArgsWithDefaults::CreateSpec() {
  return tables::WinscopeArgsWithDefaultsTable::kSpec.ToUntypedDataframeSpec();
}

std::string WinscopeProtoToArgsWithDefaults::TableName() {
  return tables::WinscopeArgsWithDefaultsTable::Name();
}

uint32_t WinscopeProtoToArgsWithDefaults::GetArgumentCount() const {
  return 1;
}

class WinscopeProtoToArgsWithDefaultsPlugin
    : public Plugin<WinscopeProtoToArgsWithDefaultsPlugin,
                    winscope_importer::WinscopeImporter> {
 public:
  ~WinscopeProtoToArgsWithDefaultsPlugin() override;

  void RegisterStaticTableFunctions(
      PerfettoSqlConnection* connection,
      std::vector<std::unique_ptr<StaticTableFunction>>& fns) override {
    fns.emplace_back(std::make_unique<WinscopeProtoToArgsWithDefaults>(
        trace_context_->storage->mutable_string_pool(), connection,
        trace_context_));
  }
};

WinscopeProtoToArgsWithDefaultsPlugin::
    ~WinscopeProtoToArgsWithDefaultsPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<WinscopeProtoToArgsWithDefaultsPlugin>();
      },
      WinscopeProtoToArgsWithDefaultsPlugin::kPluginId,
      WinscopeProtoToArgsWithDefaultsPlugin::kDepIds.data(),
      WinscopeProtoToArgsWithDefaultsPlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::winscope_proto_to_args_with_defaults
