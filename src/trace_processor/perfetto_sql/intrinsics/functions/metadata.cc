// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/trace_processor/perfetto_sql/intrinsics/functions/metadata.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"

namespace perfetto::trace_processor {

namespace {

constexpr uint32_t kNullId = 0xFFFFFFFF;

struct MetadataUserData {
  explicit MetadataUserData(const TraceStorage* _storage) : storage(_storage) {}
  const TraceStorage* storage;
};

struct StringTrait {
  using Type = StringId;
  static std::optional<Type> Get(
      const tables::MetadataTable::ConstIterator& it) {
    return it.str_value();
  }
  static void Report(sqlite3_context* ctx,
                     const TraceStorage* storage,
                     Type val) {
    sqlite::result::StaticString(ctx, storage->GetString(val).c_str());
  }
};

struct IntTrait {
  using Type = int64_t;
  static std::optional<Type> Get(
      const tables::MetadataTable::ConstIterator& it) {
    return it.int_value();
  }
  static void Report(sqlite3_context* ctx, const TraceStorage*, Type val) {
    sqlite::result::Long(ctx, val);
  }
};

template <typename ValueTrait>
struct PrimaryEntry {
  bool initialized = false;
  std::optional<typename ValueTrait::Type> value;
  std::optional<tables::MachineTable::Id> machine_id;
  std::optional<tables::TraceFileTable::Id> trace_id;

  bool IsPrimary(std::optional<tables::MachineTable::Id> m,
                 std::optional<tables::TraceFileTable::Id> t) const {
    if (!initialized) {
      return true;
    }
    // Prioritize by trace_id, then machine_id.
    // We treat NULLs as kNullId (worst entries).
    uint32_t current_t = trace_id.has_value() ? trace_id->value : kNullId;
    uint32_t new_t = t.has_value() ? t->value : kNullId;
    if (new_t < current_t) {
      return true;
    }
    if (new_t > current_t) {
      return false;
    }

    uint32_t current_m = machine_id.has_value() ? machine_id->value : kNullId;
    uint32_t new_m = m.has_value() ? m->value : kNullId;
    return new_m < current_m;
  }

  void Update(const tables::MetadataTable::ConstIterator& it) {
    if (IsPrimary(it.machine_id(), it.trace_id())) {
      value = ValueTrait::Get(it);
      machine_id = it.machine_id();
      trace_id = it.trace_id();
      initialized = true;
    }
  }
};

template <typename ValueTrait>
struct MetadataGetPrimary
    : public sqlite::Function<MetadataGetPrimary<ValueTrait>> {
  using UserData = MetadataUserData;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == 1);
    if (sqlite::value::Type(argv[0]) == sqlite::Type::kNull) {
      return sqlite::utils::ReturnNullFromFunction(ctx);
    }
    if (sqlite::value::Type(argv[0]) != sqlite::Type::kText) {
      return sqlite::utils::SetError(ctx,
                                     "metadata_get: name must be a string");
    }
    std::string_view name = sqlite::value::Text(argv[0]);
    const auto* storage =
        MetadataGetPrimary<ValueTrait>::GetUserData(ctx)->storage;
    const auto& table = storage->metadata_table();

    std::optional<StringId> name_id = storage->string_pool().GetId(name);
    if (!name_id.has_value()) {
      return sqlite::utils::ReturnNullFromFunction(ctx);
    }

    PrimaryEntry<ValueTrait> primary;
    for (auto it = table.IterateRows(); it; ++it) {
      if (it.name() == *name_id) {
        primary.Update(it);
      }
    }

    if (primary.value.has_value()) {
      ValueTrait::Report(ctx, storage, *primary.value);
    }
  }
};

template <typename IdTable, typename ValueTrait>
struct MetadataGetByKey
    : public sqlite::Function<MetadataGetByKey<IdTable, ValueTrait>> {
  using UserData = MetadataUserData;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == 2);
    if (sqlite::value::Type(argv[0]) == sqlite::Type::kNull ||
        sqlite::value::Type(argv[1]) == sqlite::Type::kNull) {
      return sqlite::utils::ReturnNullFromFunction(ctx);
    }
    if (sqlite::value::Type(argv[0]) != sqlite::Type::kInteger) {
      return sqlite::utils::SetError(
          ctx, "metadata_get_by_key: id must be an integer");
    }
    if (sqlite::value::Type(argv[1]) != sqlite::Type::kText) {
      return sqlite::utils::SetError(
          ctx, "metadata_get_by_key: name must be a string");
    }

    auto id = typename IdTable::Id(
        static_cast<uint32_t>(sqlite::value::Int64(argv[0])));
    std::string_view name = sqlite::value::Text(argv[1]);
    const auto* storage =
        MetadataGetByKey<IdTable, ValueTrait>::GetUserData(ctx)->storage;
    const auto& table = storage->metadata_table();

    std::optional<StringId> name_id = storage->string_pool().GetId(name);
    if (!name_id.has_value()) {
      return sqlite::utils::ReturnNullFromFunction(ctx);
    }

    for (auto it = table.IterateRows(); it; ++it) {
      if (it.name() != *name_id) {
        continue;
      }

      bool id_match = false;
      if constexpr (std::is_same_v<IdTable, tables::MachineTable>) {
        id_match = (it.machine_id() == id);
      } else {
        static_assert(std::is_same_v<IdTable, tables::TraceFileTable>);
        id_match = (it.trace_id() == id);
      }

      if (id_match) {
        auto val = ValueTrait::Get(it);
        if (val.has_value()) {
          ValueTrait::Report(ctx, storage, *val);
          return;
        }
      }
    }
  }
};

struct MetadataGetStr : public MetadataGetPrimary<StringTrait> {
  static constexpr char kName[] = "metadata_get_str";
  static constexpr int kArgCount = 1;
};

struct MetadataGetInt : public MetadataGetPrimary<IntTrait> {
  static constexpr char kName[] = "metadata_get_int";
  static constexpr int kArgCount = 1;
};

struct MetadataGetMachineStr
    : public MetadataGetByKey<tables::MachineTable, StringTrait> {
  static constexpr char kName[] = "metadata_get_machine_str";
  static constexpr int kArgCount = 2;
};

struct MetadataGetMachineInt
    : public MetadataGetByKey<tables::MachineTable, IntTrait> {
  static constexpr char kName[] = "metadata_get_machine_int";
  static constexpr int kArgCount = 2;
};

struct MetadataGetTraceStr
    : public MetadataGetByKey<tables::TraceFileTable, StringTrait> {
  static constexpr char kName[] = "metadata_get_trace_str";
  static constexpr int kArgCount = 2;
};

struct MetadataGetTraceInt
    : public MetadataGetByKey<tables::TraceFileTable, IntTrait> {
  static constexpr char kName[] = "metadata_get_trace_int";
  static constexpr int kArgCount = 2;
};

}  // namespace

base::Status RegisterMetadataFunctions(PerfettoSqlEngine& engine,
                                       TraceStorage* storage) {
  RETURN_IF_ERROR(engine.RegisterFunction<MetadataGetStr>(
      std::make_unique<MetadataUserData>(storage)));
  RETURN_IF_ERROR(engine.RegisterFunction<MetadataGetInt>(
      std::make_unique<MetadataUserData>(storage)));
  RETURN_IF_ERROR(engine.RegisterFunction<MetadataGetMachineStr>(
      std::make_unique<MetadataUserData>(storage)));
  RETURN_IF_ERROR(engine.RegisterFunction<MetadataGetMachineInt>(
      std::make_unique<MetadataUserData>(storage)));
  RETURN_IF_ERROR(engine.RegisterFunction<MetadataGetTraceStr>(
      std::make_unique<MetadataUserData>(storage)));
  return engine.RegisterFunction<MetadataGetTraceInt>(
      std::make_unique<MetadataUserData>(storage));
}

}  // namespace perfetto::trace_processor
