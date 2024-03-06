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

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/dfs.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/trace_processor/basic_types.h"
#include "protos/perfetto/trace_processor/metrics_impl.pbzero.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/tables_py.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {
namespace tables {
DfsTable::~DfsTable() = default;
}  // namespace tables

namespace {

using Destinations = std::vector<uint32_t>;

base::StatusOr<std::vector<Destinations>> ParseSourceToDestionationsMap(
    protos::pbzero::RepeatedBuilderResult::Decoder& source,
    protos::pbzero::RepeatedBuilderResult::Decoder& dest) {
  std::vector<Destinations> source_to_destinations_map;
  bool parse_error = false;
  auto source_node_ids = source.int_values(&parse_error);
  auto dest_node_ids = dest.int_values(&parse_error);
  for (; source_node_ids && dest_node_ids; ++source_node_ids, ++dest_node_ids) {
    source_to_destinations_map.resize(
        std::max(source_to_destinations_map.size(),
                 std::max(static_cast<size_t>(*source_node_ids + 1),
                          static_cast<size_t>(*dest_node_ids + 1))));
    source_to_destinations_map[static_cast<uint32_t>(*source_node_ids)]
        .push_back(static_cast<uint32_t>(*dest_node_ids));
  }
  if (parse_error) {
    return base::ErrStatus("Failed while parsing source or dest ids");
  }
  if (static_cast<bool>(source_node_ids) != static_cast<bool>(dest_node_ids)) {
    return base::ErrStatus(
        "dfs: length of source and destination columns is not the same");
  }
  return source_to_destinations_map;
}

void DfsImpl(tables::DfsTable* table,
             const std::vector<Destinations>& source_to_destinations_map,
             std::vector<uint8_t>& seen_node_ids,
             uint32_t start_id) {
  struct StackState {
    uint32_t id;
    std::optional<uint32_t> parent_id;
  };

  std::vector<StackState> stack{{start_id, std::nullopt}};
  while (!stack.empty()) {
    StackState stack_state = stack.back();
    stack.pop_back();

    if (seen_node_ids[stack_state.id]) {
      continue;
    }
    seen_node_ids[stack_state.id] = true;

    tables::DfsTable::Row row;
    row.node_id = stack_state.id;
    row.parent_node_id = stack_state.parent_id;
    table->Insert(row);

    PERFETTO_DCHECK(stack_state.id < source_to_destinations_map.size());
    const auto& children = source_to_destinations_map[stack_state.id];
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.emplace_back(StackState{*it, stack_state.id});
    }
  }
}

}  // namespace

Dfs::Dfs(StringPool* pool) : pool_(pool) {}
Dfs::~Dfs() = default;

Table::Schema Dfs::CreateSchema() {
  return tables::DfsTable::ComputeStaticSchema();
}

std::string Dfs::TableName() {
  return tables::DfsTable::Name();
}

uint32_t Dfs::EstimateRowCount() {
  // TODO(lalitm): improve this estimate.
  return 1024;
}

base::StatusOr<std::unique_ptr<Table>> Dfs::ComputeTable(
    const std::vector<SqlValue>& arguments) {
  PERFETTO_CHECK(arguments.size() == 3);

  const SqlValue& raw_source_ids = arguments[0];
  const SqlValue& raw_dest_ids = arguments[1];
  const SqlValue& raw_start_node = arguments[2];
  if (raw_source_ids.is_null() && raw_dest_ids.is_null() &&
      raw_start_node.is_null()) {
    return std::unique_ptr<Table>(std::make_unique<tables::DfsTable>(pool_));
  }
  if (raw_source_ids.is_null() || raw_dest_ids.is_null() ||
      raw_start_node.is_null()) {
    return base::ErrStatus(
        "dfs: either all arguments should be null or none should be");
  }
  if (raw_source_ids.type != SqlValue::kBytes) {
    return base::ErrStatus("dfs: source_node_ids should be a repeated field");
  }
  if (raw_dest_ids.type != SqlValue::kBytes) {
    return base::ErrStatus("dfs: dest_node_ids should be a repeated field");
  }
  if (raw_start_node.type != SqlValue::kLong) {
    return base::ErrStatus("dfs: start_node_id should be an integer");
  }

  protos::pbzero::ProtoBuilderResult::Decoder proto_source_ids(
      static_cast<const uint8_t*>(raw_source_ids.AsBytes()),
      raw_source_ids.bytes_count);
  if (!proto_source_ids.is_repeated()) {
    return base::ErrStatus(
        "dfs: source_node_ids is not generated by RepeatedField function");
  }
  protos::pbzero::RepeatedBuilderResult::Decoder source_ids(
      proto_source_ids.repeated());

  protos::pbzero::ProtoBuilderResult::Decoder proto_dest_ids(
      static_cast<const uint8_t*>(raw_dest_ids.AsBytes()),
      raw_dest_ids.bytes_count);
  if (!proto_dest_ids.is_repeated()) {
    return base::ErrStatus(
        "dfs: dest_node_ids is not generated by RepeatedField function");
  }
  protos::pbzero::RepeatedBuilderResult::Decoder dest_ids(
      proto_dest_ids.repeated());

  ASSIGN_OR_RETURN(auto map,
                   ParseSourceToDestionationsMap(source_ids, dest_ids));
  uint32_t start_node_id = static_cast<uint32_t>(raw_start_node.AsLong());
  if (start_node_id >= map.size()) {
    return std::unique_ptr<Table>(std::make_unique<tables::DfsTable>(pool_));
  }

  std::vector<uint8_t> seen_node_ids(map.size());
  auto table = std::make_unique<tables::DfsTable>(pool_);
  DfsImpl(table.get(), map, seen_node_ids, start_node_id);
  return std::unique_ptr<Table>(std::move(table));
}

}  // namespace perfetto::trace_processor
