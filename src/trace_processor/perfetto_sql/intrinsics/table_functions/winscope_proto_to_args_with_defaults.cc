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

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/winscope_proto_to_args_with_defaults.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/proto_to_args_parser.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/trace_processor/util/winscope_proto_mapping.h"

namespace perfetto::trace_processor {
namespace tables {
WinscopeArgsWithDefaultsTable::~WinscopeArgsWithDefaultsTable() = default;
}  // namespace tables

namespace {
constexpr char kDeinternError[] = "STRING DE-INTERNING ERROR";

// Interned data stored in table with columns:
// - base64_proto_id
// - flat_key
// - iid
// - deinterned_value
// Mapping reconstructed using nested FlatHashMaps to optionally
// deintern strings from proto data.
using ProtoId = uint32_t;
using FlatKey = StringPool::Id;
using Iid = uint64_t;
using DeinternedValue = StringPool::Id;

using DeinternedIids = base::FlatHashMap<Iid, DeinternedValue>;
using InternedData = base::FlatHashMap<FlatKey, DeinternedIids>;
using ProtoToInternedData = base::FlatHashMap<ProtoId, InternedData>;

ProtoToInternedData GetProtoToInternedData(const std::string& table_name,
                                           TraceStorage* storage,
                                           StringPool* pool) {
  ProtoToInternedData proto_to_interned_data;
  auto interned_data_table =
      util::winscope_proto_mapping::GetInternedDataTable(table_name, storage);
  if (interned_data_table) {
    const Table* table = interned_data_table.value();
    const auto proto_id_idx =
        table->ColumnIdxFromName("base64_proto_id").value();
    const auto flat_key_idx = table->ColumnIdxFromName("flat_key").value();
    const auto iid_idx = table->ColumnIdxFromName("iid").value();
    const auto deinterned_value_idx =
        table->ColumnIdxFromName("deinterned_value").value();

    for (auto it = table->IterateRows(); it; ++it) {
      const auto proto_id =
          static_cast<uint32_t>(it.Get(proto_id_idx).AsLong());
      const auto flat_key = pool->InternString(
          base::StringView(std::string(it.Get(flat_key_idx).AsString())));
      const auto iid = static_cast<uint64_t>(it.Get(iid_idx).AsLong());
      const auto deinterned_value = pool->InternString(base::StringView(
          std::string(it.Get(deinterned_value_idx).AsString())));

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

class Delegate : public util::ProtoToArgsParser::Delegate {
 public:
  using Key = util::ProtoToArgsParser::Key;
  explicit Delegate(StringPool* pool,
                    const uint32_t base64_proto_id,
                    tables::WinscopeArgsWithDefaultsTable* table,
                    KeyToRowMap* key_to_row,
                    const InternedData* interned_data)
      : pool_(pool),
        base64_proto_id_(base64_proto_id),
        table_(table),
        key_to_row_(key_to_row),
        interned_data_(interned_data) {}

  void AddInteger(const Key& key, int64_t res) override {
    if (TryAddDeinternedString(key, static_cast<uint64_t>(res))) {
      return;
    }
    RowReference r = GetOrCreateRow(key);
    r.set_int_value(res);
  }
  void AddUnsignedInteger(const Key& key, uint64_t res) override {
    if (TryAddDeinternedString(key, static_cast<uint64_t>(res))) {
      return;
    }
    RowReference r = GetOrCreateRow(key);
    r.set_int_value(int64_t(res));
  }
  void AddString(const Key& key, const protozero::ConstChars& res) override {
    RowReference r = GetOrCreateRow(key);
    r.set_string_value(
        pool_->InternString(base::StringView((res.ToStdString()))));
  }
  void AddString(const Key& key, const std::string& res) override {
    RowReference r = GetOrCreateRow(key);
    r.set_string_value(pool_->InternString(base::StringView(res)));
  }
  void AddDouble(const Key& key, double res) override {
    RowReference r = GetOrCreateRow(key);
    r.set_real_value(res);
  }
  void AddBoolean(const Key& key, bool res) override {
    RowReference r = GetOrCreateRow(key);
    r.set_int_value(res);
  }
  void AddBytes(const Key& key, const protozero::ConstBytes& res) override {
    RowReference r = GetOrCreateRow(key);
    r.set_string_value(
        pool_->InternString(base::StringView((res.ToStdString()))));
  }
  void AddNull(const Key& key) override { GetOrCreateRow(key); }
  void AddPointer(const Key&, uint64_t) override {
    PERFETTO_FATAL("Unsupported");
  }
  bool AddJson(const Key&, const protozero::ConstChars&) override {
    PERFETTO_FATAL("Unsupported");
  }
  size_t GetArrayEntryIndex(const std::string&) override {
    PERFETTO_FATAL("Unsupported");
  }
  size_t IncrementArrayEntryIndex(const std::string&) override {
    PERFETTO_FATAL("Unsupported");
  }
  PacketSequenceStateGeneration* seq_state() override { return nullptr; }

  bool ShouldAddDefaultArg(const Key& key) override {
    if (!key_to_row_) {
      return true;
    }
    auto key_id = pool_->InternString(base::StringView(key.key));
    auto pos = key_to_row_->find(key_id);
    return pos == key_to_row_->end();
  }

 private:
  InternedMessageView* GetInternedMessageView(uint32_t, uint64_t) override {
    return nullptr;
  }

  RowReference GetOrCreateRow(const Key& key) {
    RowId row_id;
    if (!key_to_row_) {
      Row new_row;
      row_id = table_->Insert(new_row).id;
    } else {
      auto key_id = pool_->InternString(base::StringView(key.key));
      auto pos = key_to_row_->find(key_id);
      if (pos != key_to_row_->end()) {
        row_id = pos->second;
      } else {
        Row new_row;
        row_id = table_->Insert(new_row).id;
        key_to_row_->insert({key_id, row_id});
      }
    }

    auto row = table_->FindById(row_id).value();
    row.set_key(pool_->InternString(base::StringView(key.key)));
    row.set_flat_key(pool_->InternString(base::StringView(key.flat_key)));
    row.set_base64_proto_id(base64_proto_id_);
    return row;
  }

  bool TryAddDeinternedString(const Key& key, uint64_t iid) {
    if (!interned_data_ || !base::EndsWith(key.key, "_iid")) {
      return false;
    }
    const auto deinterned_key =
        Key{key.flat_key.substr(0, key.flat_key.size() - 4),
            key.key.substr(0, key.key.size() - 4)};
    const auto deinterned_value = TryDeinternString(key, iid);
    if (!deinterned_value) {
      AddString(deinterned_key,
                protozero::ConstChars{kDeinternError, sizeof(kDeinternError)});
      return false;
    }
    AddString(deinterned_key, *deinterned_value);
    return true;
  }

  std::optional<std::string> TryDeinternString(const Key& key, uint64_t iid) {
    DeinternedIids* deinterned_iids = interned_data_->Find(
        pool_->InternString(base::StringView(key.flat_key)));
    if (!deinterned_iids) {
      return std::nullopt;
    }
    auto* deinterned_value = deinterned_iids->Find(iid);
    if (!deinterned_value) {
      return std::nullopt;
    }
    return pool_->Get(*(deinterned_value)).data();
  }

  StringPool* pool_;
  const uint32_t base64_proto_id_;
  tables::WinscopeArgsWithDefaultsTable* table_;
  KeyToRowMap* key_to_row_;
  const InternedData* interned_data_;
};

base::Status InsertRows(
    const Table& static_table,
    tables::WinscopeArgsWithDefaultsTable* inflated_args_table,
    const std::string& proto_name,
    const std::vector<uint32_t>* allowed_fields,
    const std::string* group_id_col_name,
    DescriptorPool& descriptor_pool,
    StringPool* string_pool,
    const ProtoToInternedData& proto_to_interned_data) {
  util::ProtoToArgsParser args_parser{descriptor_pool};
  const auto base64_proto_id_col_idx =
      static_table.ColumnIdxFromName("base64_proto_id").value();

  std::optional<uint32_t> group_id_col_idx;
  if (group_id_col_name) {
    group_id_col_idx = static_table.ColumnIdxFromName(*group_id_col_name);
  }

  std::unordered_set<uint32_t> inflated_protos;
  std::unordered_map<uint32_t, KeyToRowMap> group_id_to_key_row_map;
  for (auto it = static_table.IterateRows(); it; ++it) {
    const auto base64_proto_id =
        static_cast<uint32_t>(it.Get(base64_proto_id_col_idx).AsLong());
    if (inflated_protos.count(base64_proto_id) > 0) {
      continue;
    }
    inflated_protos.insert(base64_proto_id);

    const auto raw_proto =
        string_pool->Get(StringPool::Id::Raw(base64_proto_id));
    const auto blob = *base::Base64Decode(raw_proto);
    const auto cb = protozero::ConstBytes{
        reinterpret_cast<const uint8_t*>(blob.data()), blob.size()};

    KeyToRowMap* key_to_row = nullptr;
    if (group_id_col_idx.has_value()) {
      auto group_id = static_cast<uint32_t>(it.Get(*group_id_col_idx).AsLong());
      auto pos = group_id_to_key_row_map.find(group_id);
      if (pos != group_id_to_key_row_map.end()) {
        key_to_row = &(pos->second);
      } else {
        key_to_row = &(group_id_to_key_row_map[group_id]);
      }
    }

    InternedData* interned_data = proto_to_interned_data.Find(base64_proto_id);
    Delegate delegate(string_pool, base64_proto_id, inflated_args_table,
                      key_to_row, interned_data);
    RETURN_IF_ERROR(args_parser.ParseMessage(cb, proto_name, allowed_fields,
                                             delegate, nullptr, true));
  }
  return base::OkStatus();
}
}  // namespace

WinscopeProtoToArgsWithDefaults::WinscopeProtoToArgsWithDefaults(
    StringPool* string_pool,
    const PerfettoSqlEngine* engine,
    TraceProcessorContext* context)
    : string_pool_(string_pool), engine_(engine), context_(context) {}

base::StatusOr<std::unique_ptr<Table>>
WinscopeProtoToArgsWithDefaults::ComputeTable(
    const std::vector<SqlValue>& arguments) {
  PERFETTO_CHECK(arguments.size() == 1);
  if (arguments[0].type != SqlValue::kString) {
    return base::ErrStatus(
        "__intrinsic_winscope_proto_to_args_with_defaults takes table name as "
        "a string.");
  }
  std::string table_name = arguments[0].AsString();

  const Table* static_table = engine_->GetTableOrNull(table_name);
  if (!static_table) {
    return base::ErrStatus("Failed to find %s table.", table_name.c_str());
  }

  std::string proto_name;
  ASSIGN_OR_RETURN(proto_name,
                   util::winscope_proto_mapping::GetProtoName(table_name));

  auto table =
      std::make_unique<tables::WinscopeArgsWithDefaultsTable>(string_pool_);
  auto allowed_fields =
      util::winscope_proto_mapping::GetAllowedFields(table_name);
  auto group_id_col_name =
      util::winscope_proto_mapping::GetGroupIdColName(table_name);
  auto proto_to_interned_data =
      GetProtoToInternedData(table_name, context_->storage.get(), string_pool_);

  RETURN_IF_ERROR(InsertRows(
      *static_table, table.get(), proto_name,
      allowed_fields ? &allowed_fields.value() : nullptr,
      group_id_col_name ? &group_id_col_name.value() : nullptr,
      *context_->descriptor_pool_, string_pool_, proto_to_interned_data));

  return std::unique_ptr<Table>(std::move(table));
}

Table::Schema WinscopeProtoToArgsWithDefaults::CreateSchema() {
  return tables::WinscopeArgsWithDefaultsTable::ComputeStaticSchema();
}

std::string WinscopeProtoToArgsWithDefaults::TableName() {
  return tables::WinscopeArgsWithDefaultsTable::Name();
}

uint32_t WinscopeProtoToArgsWithDefaults::EstimateRowCount() {
  // 100 inflated args per 100 elements per 100 entries
  return 1000000;
}
}  // namespace perfetto::trace_processor
