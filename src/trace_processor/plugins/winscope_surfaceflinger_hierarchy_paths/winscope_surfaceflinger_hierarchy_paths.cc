/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/plugins/winscope_surfaceflinger_hierarchy_paths/winscope_surfaceflinger_hierarchy_paths.h"

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/plugins/winscope_importer/winscope_importer.h"
#include "src/trace_processor/plugins/winscope_surfaceflinger_hierarchy_paths/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/winscope_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::winscope_surfaceflinger_hierarchy_paths {

namespace {

using LayerTable = tables::SurfaceFlingerLayerTable;

class WinscopeSurfaceFlingerHierarchyPaths : public StaticTableFunction {
 public:
  class Cursor : public StaticTableFunction::Cursor {
   public:
    explicit Cursor(StringPool* string_pool,
                    const PerfettoSqlConnection* connection);
    bool Run(const std::vector<SqlValue>& arguments) override;

   private:
    const PerfettoSqlConnection* engine_ = nullptr;
    tables::WinscopeSurfaceFlingerHierarchyPathTable table_;
  };

  explicit WinscopeSurfaceFlingerHierarchyPaths(StringPool*,
                                                const PerfettoSqlConnection*);

  std::unique_ptr<StaticTableFunction::Cursor> MakeCursor() override;
  dataframe::DataframeSpec CreateSpec() override;
  std::string TableName() override;
  uint32_t GetArgumentCount() const override;

 private:
  StringPool* string_pool_ = nullptr;
  const PerfettoSqlConnection* engine_ = nullptr;
};

struct Layer {
  uint32_t snapshot_id;
  std::optional<int64_t> id;
  int64_t parent;
};

Layer ReadLayer(const dataframe::Dataframe& layer_table, uint32_t row) {
  constexpr auto kSpec = LayerTable::kSpec;
  return Layer{
      layer_table.GetCellUnchecked<LayerTable::ColumnIndex::snapshot_id>(kSpec,
                                                                         row),
      layer_table.GetCellUnchecked<LayerTable::ColumnIndex::layer_id>(kSpec,
                                                                      row),
      // A layer without a parent reads as parent 0, the proto default.
      layer_table.GetCellUnchecked<LayerTable::ColumnIndex::parent>(kSpec, row)
          .value_or(0),
  };
}

std::vector<int64_t> GetHierarchyPath(
    const Layer& layer,
    const std::unordered_map<int64_t, int64_t>& parent_by_id) {
  std::vector<int64_t> hierarchy_path = {*layer.id};
  auto pos = parent_by_id.find(layer.parent);
  while (pos != parent_by_id.end()) {
    auto id = pos->first;
    auto parent = pos->second;
    if (id == parent) {
      // handles recursive hierarchies
      break;
    }
    hierarchy_path.push_back(id);
    pos = parent_by_id.find(parent);
  }
  return hierarchy_path;
}

// Layer rows are inserted one snapshot at a time, so each snapshot's layers are
// contiguous.
base::Status InsertRows(
    const dataframe::Dataframe& layer_table,
    tables::WinscopeSurfaceFlingerHierarchyPathTable* paths_table) {
  const uint32_t row_count = layer_table.row_count();
  uint32_t begin = 0;
  while (begin < row_count) {
    const uint32_t snapshot_id = ReadLayer(layer_table, begin).snapshot_id;

    std::unordered_map<int64_t, int64_t> parent_by_id;
    uint32_t end = begin;
    for (; end < row_count; ++end) {
      const Layer layer = ReadLayer(layer_table, end);
      if (layer.snapshot_id != snapshot_id) {
        break;
      }
      if (layer.id) {
        parent_by_id.emplace(*layer.id, layer.parent);
      }
    }

    for (uint32_t i = begin; i < end; ++i) {
      const Layer layer = ReadLayer(layer_table, i);
      if (!layer.id) {
        continue;
      }
      auto layer_id = static_cast<uint32_t>(*layer.id);

      auto path = GetHierarchyPath(layer, parent_by_id);
      for (auto path_it = path.rbegin(); path_it != path.rend(); ++path_it) {
        tables::WinscopeSurfaceFlingerHierarchyPathTable::Row row;
        row.snapshot_id = snapshot_id;
        row.layer_id = layer_id;
        row.ancestor_id = static_cast<uint32_t>(*path_it);
        paths_table->Insert(row);
      }
    }
    begin = end;
  }
  return base::OkStatus();
}

WinscopeSurfaceFlingerHierarchyPaths::Cursor::Cursor(
    StringPool* string_pool,
    const PerfettoSqlConnection* connection)
    : engine_(connection), table_(string_pool) {}

bool WinscopeSurfaceFlingerHierarchyPaths::Cursor::Run(
    const std::vector<SqlValue>& arguments) {
  PERFETTO_DCHECK(arguments.size() == 0);
  auto table_name = LayerTable::Name();
  const dataframe::Dataframe* static_table_from_connection =
      engine_->GetDataframeOrNull(table_name);
  if (!static_table_from_connection) {
    return OnFailure(base::ErrStatus("Failed to find %s table.",
                                     std::string(table_name).c_str()));
  }

  table_.Clear();

  base::Status status = InsertRows(*static_table_from_connection, &table_);
  if (!status.ok()) {
    return OnFailure(status);
  }
  return OnSuccess(&table_.dataframe());
}

WinscopeSurfaceFlingerHierarchyPaths::WinscopeSurfaceFlingerHierarchyPaths(
    StringPool* string_pool,
    const PerfettoSqlConnection* connection)
    : string_pool_(string_pool), engine_(connection) {}

std::unique_ptr<StaticTableFunction::Cursor>
WinscopeSurfaceFlingerHierarchyPaths::MakeCursor() {
  return std::make_unique<Cursor>(string_pool_, engine_);
}

dataframe::DataframeSpec WinscopeSurfaceFlingerHierarchyPaths::CreateSpec() {
  return tables::WinscopeSurfaceFlingerHierarchyPathTable::kSpec
      .ToUntypedDataframeSpec();
}

std::string WinscopeSurfaceFlingerHierarchyPaths::TableName() {
  return tables::WinscopeSurfaceFlingerHierarchyPathTable::Name();
}

uint32_t WinscopeSurfaceFlingerHierarchyPaths::GetArgumentCount() const {
  return 0;
}

class WinscopeSurfaceFlingerHierarchyPathsPlugin
    : public Plugin<WinscopeSurfaceFlingerHierarchyPathsPlugin,
                    winscope_importer::WinscopeImporter> {
 public:
  ~WinscopeSurfaceFlingerHierarchyPathsPlugin() override;

  void RegisterStaticTableFunctions(
      PerfettoSqlConnection* connection,
      std::vector<std::unique_ptr<StaticTableFunction>>& fns) override {
    fns.emplace_back(std::make_unique<WinscopeSurfaceFlingerHierarchyPaths>(
        trace_context_->storage->mutable_string_pool(), connection));
  }
};

WinscopeSurfaceFlingerHierarchyPathsPlugin::
    ~WinscopeSurfaceFlingerHierarchyPathsPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<WinscopeSurfaceFlingerHierarchyPathsPlugin>();
      },
      WinscopeSurfaceFlingerHierarchyPathsPlugin::kPluginId,
      WinscopeSurfaceFlingerHierarchyPathsPlugin::kDepIds.data(),
      WinscopeSurfaceFlingerHierarchyPathsPlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace
   // perfetto::trace_processor::winscope_surfaceflinger_hierarchy_paths
