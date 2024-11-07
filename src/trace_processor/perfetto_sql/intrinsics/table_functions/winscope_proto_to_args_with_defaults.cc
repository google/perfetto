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

#include "perfetto/base/status.h"
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/tables_py.h"
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
using Row = tables::WinscopeArgsWithDefaultsTable::Row;

class Delegate : public util::ProtoToArgsParser::Delegate {
 public:
  using Key = util::ProtoToArgsParser::Key;
  explicit Delegate(StringPool* pool,
                    const uint32_t base64_proto_id,
                    tables::WinscopeArgsWithDefaultsTable* table)
      : pool_(pool), base64_proto_id_(base64_proto_id), table_(table) {}

  void AddInteger(const Key& key, int64_t res) override {
    Row r;
    r.int_value = res;
    SetColumnsAndInsertRow(key, r);
  }
  void AddUnsignedInteger(const Key& key, uint64_t res) override {
    Row r;
    r.int_value = res;
    SetColumnsAndInsertRow(key, r);
  }
  void AddString(const Key& key, const protozero::ConstChars& res) override {
    Row r;
    r.string_value = pool_->InternString(base::StringView((res.ToStdString())));
    SetColumnsAndInsertRow(key, r);
  }
  void AddString(const Key& key, const std::string& res) override {
    Row r;
    r.string_value = pool_->InternString(base::StringView(res));
    SetColumnsAndInsertRow(key, r);
  }
  void AddDouble(const Key& key, double res) override {
    Row r;
    r.real_value = res;
    SetColumnsAndInsertRow(key, r);
  }
  void AddBoolean(const Key& key, bool res) override {
    Row r;
    r.int_value = res;
    SetColumnsAndInsertRow(key, r);
  }
  void AddBytes(const Key& key, const protozero::ConstBytes& res) override {
    Row r;
    r.string_value = pool_->InternString(base::StringView((res.ToStdString())));
    SetColumnsAndInsertRow(key, r);
  }
  void AddNull(const Key& key) override {
    Row r;
    SetColumnsAndInsertRow(key, r);
  }
  void AddPointer(const Key&, const void*) override {
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

 private:
  InternedMessageView* GetInternedMessageView(uint32_t, uint64_t) override {
    return nullptr;
  }

  void SetColumnsAndInsertRow(const Key& key, Row& row) {
    row.key = pool_->InternString(base::StringView(key.key));
    row.flat_key = pool_->InternString(base::StringView(key.flat_key));
    row.base64_proto_id = base64_proto_id_;
    table_->Insert(row);
  }

  StringPool* pool_;
  const uint32_t base64_proto_id_;
  tables::WinscopeArgsWithDefaultsTable* table_;
};

base::Status InsertRows(
    const Table& static_table,
    tables::WinscopeArgsWithDefaultsTable* inflated_args_table,
    const std::string& proto_name,
    const std::vector<uint32_t>* allowed_fields,
    DescriptorPool& descriptor_pool,
    StringPool* string_pool) {
  util::ProtoToArgsParser args_parser{descriptor_pool};
  const auto base64_proto_id_col_idx =
      static_table.ColumnIdxFromName("base64_proto_id").value();
  const auto base_64_proto_col_idx =
      static_table.ColumnIdxFromName("base64_proto").value();

  std::unordered_set<uint32_t> inflated_protos;
  for (auto it = static_table.IterateRows(); it; ++it) {
    const auto base64_proto_id =
        static_cast<uint32_t>(it.Get(base64_proto_id_col_idx).AsLong());
    if (inflated_protos.count(base64_proto_id) > 0) {
      continue;
    }
    inflated_protos.insert(base64_proto_id);
    const auto* raw_proto = it.Get(base_64_proto_col_idx).AsString();
    const auto blob = *base::Base64Decode(raw_proto);
    const auto cb = protozero::ConstBytes{
        reinterpret_cast<const uint8_t*>(blob.data()), blob.size()};
    Delegate delegate(string_pool, base64_proto_id, inflated_args_table);
    RETURN_IF_ERROR(args_parser.ParseMessage(cb, proto_name, allowed_fields,
                                             delegate, nullptr, true));
  }
  return base::OkStatus();
}
}  // namespace

WinscopeProtoToArgsWithDefaults::WinscopeProtoToArgsWithDefaults(
    StringPool* string_pool,
    PerfettoSqlEngine* engine,
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

  const Table* static_table = engine_->GetStaticTableOrNull(table_name);
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
  RETURN_IF_ERROR(InsertRows(*static_table, table.get(), proto_name,
                             allowed_fields ? &allowed_fields.value() : nullptr,
                             *context_->descriptor_pool_, string_pool_));

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
