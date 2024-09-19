/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/ancestor.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/column_storage.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {
namespace tables {

AncestorSliceTable::~AncestorSliceTable() = default;
AncestorStackProfileCallsiteTable::~AncestorStackProfileCallsiteTable() =
    default;
AncestorSliceByStackTable::~AncestorSliceByStackTable() = default;

}  // namespace tables

namespace {

template <typename T>
base::Status GetAncestors(
    const T& table,
    typename T::Id starting_id,
    std::vector<typename T::RowNumber>& row_numbers_accumulator) {
  auto start_ref = table.FindById(starting_id);
  if (!start_ref) {
    return base::ErrStatus("no row with id %" PRIu32 "",
                           static_cast<uint32_t>(starting_id.value));
  }

  // It's important we insert directly into |row_numbers_accumulator| and not
  // overwrite it because we expect the existing elements in
  // |row_numbers_accumulator| to be preserved.
  auto maybe_parent_id = start_ref->parent_id();
  while (maybe_parent_id) {
    auto ref = *table.FindById(*maybe_parent_id);
    row_numbers_accumulator.emplace_back(ref.ToRowNumber());
    // Update the loop variable by looking up the next parent_id.
    maybe_parent_id = ref.parent_id();
  }
  // We traverse the tree in reverse id order. To ensure we meet the
  // requirements of the extension vectors being sorted, ensure that we reverse
  // the row numbers to be in id order.
  std::reverse(row_numbers_accumulator.begin(), row_numbers_accumulator.end());
  return base::OkStatus();
}

template <typename ChildTable, typename ConstraintType, typename ParentTable>
std::unique_ptr<Table> ExtendWithStartId(
    ConstraintType constraint_value,
    const ParentTable& table,
    std::vector<typename ParentTable::RowNumber> parent_rows) {
  ColumnStorage<ConstraintType> start_ids;
  for (uint32_t i = 0; i < parent_rows.size(); ++i)
    start_ids.Append(constraint_value);
  return ChildTable::SelectAndExtendParent(table, std::move(parent_rows),
                                           std::move(start_ids));
}

template <typename ChildTable, typename ParentTable>
base::StatusOr<std::unique_ptr<Table>> BuildAncestorsTable(
    typename ParentTable::Id id,
    const ParentTable& table) {
  // Build up all the parents row ids.
  std::vector<typename ParentTable::RowNumber> ancestors;
  RETURN_IF_ERROR(GetAncestors(table, id, ancestors));
  return ExtendWithStartId<ChildTable>(id.value, table, std::move(ancestors));
}

}  // namespace

Ancestor::Ancestor(Type type, const TraceStorage* storage)
    : type_(type), storage_(storage) {}

base::StatusOr<std::unique_ptr<Table>> Ancestor::ComputeTable(
    const std::vector<SqlValue>& arguments) {
  PERFETTO_CHECK(arguments.size() == 1);

  if (arguments[0].is_null()) {
    // Nothing matches a null id so return an empty table.
    switch (type_) {
      case Type::kSlice:
        return std::unique_ptr<Table>(
            tables::AncestorSliceTable::SelectAndExtendParent(
                storage_->slice_table(), {}, {}));
      case Type::kStackProfileCallsite:
        return std::unique_ptr<Table>(
            tables::AncestorStackProfileCallsiteTable::SelectAndExtendParent(
                storage_->stack_profile_callsite_table(), {}, {}));
      case Type::kSliceByStack:
        return std::unique_ptr<Table>(
            tables::AncestorSliceByStackTable::SelectAndExtendParent(
                storage_->slice_table(), {}, {}));
    }
    return base::OkStatus();
  }
  if (arguments[0].type != SqlValue::Type::kLong) {
    return base::ErrStatus("start id should be an integer.");
  }

  int64_t start_id = arguments[0].AsLong();
  uint32_t start_id_uint = static_cast<uint32_t>(start_id);
  switch (type_) {
    case Type::kSlice:
      return BuildAncestorsTable<tables::AncestorSliceTable>(
          SliceId(start_id_uint), storage_->slice_table());

    case Type::kStackProfileCallsite:
      return BuildAncestorsTable<tables::AncestorStackProfileCallsiteTable>(
          CallsiteId(start_id_uint), storage_->stack_profile_callsite_table());

    case Type::kSliceByStack: {
      // Find the all slice ids that have the stack id and find all the
      // ancestors of the slice ids.
      const auto& slice_table = storage_->slice_table();
      Query q;
      q.constraints = {slice_table.stack_id().eq(start_id)};
      auto it = slice_table.FilterToIterator(q);
      std::vector<tables::SliceTable::RowNumber> ancestors;
      for (; it; ++it) {
        RETURN_IF_ERROR(GetAncestors(slice_table, it.id(), ancestors));
      }
      // Sort to keep the slices in timestamp order.
      std::sort(ancestors.begin(), ancestors.end());
      return ExtendWithStartId<tables::AncestorSliceByStackTable>(
          start_id, slice_table, std::move(ancestors));
    }
  }
  PERFETTO_FATAL("For GCC");
}

Table::Schema Ancestor::CreateSchema() {
  switch (type_) {
    case Type::kSlice:
      return tables::AncestorSliceTable::ComputeStaticSchema();
    case Type::kStackProfileCallsite:
      return tables::AncestorStackProfileCallsiteTable::ComputeStaticSchema();
    case Type::kSliceByStack:
      return tables::AncestorSliceByStackTable::ComputeStaticSchema();
  }
  PERFETTO_FATAL("For GCC");
}

std::string Ancestor::TableName() {
  switch (type_) {
    case Type::kSlice:
      return tables::AncestorSliceTable::Name();
    case Type::kStackProfileCallsite:
      return tables::AncestorStackProfileCallsiteTable::Name();
    case Type::kSliceByStack:
      return tables::AncestorSliceByStackTable::Name();
  }
  PERFETTO_FATAL("For GCC");
}

uint32_t Ancestor::EstimateRowCount() {
  return 1;
}

// static
std::optional<std::vector<tables::SliceTable::RowNumber>>
Ancestor::GetAncestorSlices(const tables::SliceTable& slices,
                            SliceId slice_id) {
  std::vector<tables::SliceTable::RowNumber> ret;
  auto status = GetAncestors(slices, slice_id, ret);
  if (!status.ok())
    return std::nullopt;
  return std::move(ret);  // -Wreturn-std-move-in-c++11
}

}  // namespace perfetto::trace_processor
