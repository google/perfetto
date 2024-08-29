/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/descendant.h"

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
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column_storage.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/db/typed_column.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {
namespace tables {

DescendantSliceTable::~DescendantSliceTable() = default;
DescendantSliceByStackTable::~DescendantSliceByStackTable() = default;

}  // namespace tables

namespace {

template <typename ChildTable, typename ParentTable, typename ConstraintType>
std::unique_ptr<Table> ExtendWithStartId(
    ConstraintType constraint_id,
    const ParentTable& table,
    std::vector<typename ParentTable::RowNumber> parent_rows) {
  ColumnStorage<ConstraintType> start_ids;
  for (uint32_t i = 0; i < parent_rows.size(); ++i)
    start_ids.Append(constraint_id);
  return ChildTable::SelectAndExtendParent(table, std::move(parent_rows),
                                           std::move(start_ids));
}

base::Status GetDescendants(
    const tables::SliceTable& slices,
    SliceId starting_id,
    std::vector<tables::SliceTable::RowNumber>& row_numbers_accumulator) {
  auto start_ref = slices.FindById(starting_id);
  // The query gave an invalid ID that doesn't exist in the slice table.
  if (!start_ref) {
    return base::ErrStatus("no row with id %" PRIu32 "",
                           static_cast<uint32_t>(starting_id.value));
  }

  // As an optimization, for any finished slices, we only need to consider
  // slices which started before the end of this slice (because slices on a
  // track are always perfectly stacked).
  // For unfinshed slices (i.e. -1 dur), we need to consider until the end of
  // the trace so we cannot add any similar constraint.
  Query q;
  if (start_ref->dur() >= 0) {
    q.constraints.emplace_back(
        slices.ts().le(start_ref->ts() + start_ref->dur()));
  }

  // All nested descendents must be on the same track, with a ts greater than
  // |start_ref.ts| and whose depth is larger than |start_ref|'s.
  q.constraints.emplace_back(slices.ts().ge(start_ref->ts()));
  q.constraints.emplace_back(slices.track_id().eq(start_ref->track_id().value));
  q.constraints.emplace_back(slices.depth().gt(start_ref->depth()));

  // It's important we insert directly into |row_numbers_accumulator| and not
  // overwrite it because we expect the existing elements in
  // |row_numbers_accumulator| to be preserved.

  for (auto it = slices.FilterToIterator(q); it; ++it) {
    row_numbers_accumulator.emplace_back(it.row_number());
  }
  return base::OkStatus();
}

}  // namespace

Descendant::Descendant(Type type, const TraceStorage* storage)
    : type_(type), storage_(storage) {}

base::StatusOr<std::unique_ptr<Table>> Descendant::ComputeTable(
    const std::vector<SqlValue>& arguments) {
  PERFETTO_CHECK(arguments.size() == 1);

  const auto& slices = storage_->slice_table();
  if (arguments[0].type == SqlValue::Type::kNull) {
    // Nothing matches a null id so return an empty table.
    switch (type_) {
      case Type::kSlice:
        return std::unique_ptr<Table>(
            tables::DescendantSliceTable::SelectAndExtendParent(
                storage_->slice_table(), {}, {}));
      case Type::kSliceByStack:
        return std::unique_ptr<Table>(
            tables::DescendantSliceByStackTable::SelectAndExtendParent(
                storage_->slice_table(), {}, {}));
    }
    PERFETTO_FATAL("For GCC");
  }
  if (arguments[0].type != SqlValue::Type::kLong) {
    return base::ErrStatus("start id should be an integer.");
  }

  int64_t start_id = arguments[0].AsLong();
  std::vector<tables::SliceTable::RowNumber> descendants;
  switch (type_) {
    case Type::kSlice: {
      // Build up all the children row ids.
      uint32_t start_id_uint = static_cast<uint32_t>(start_id);
      RETURN_IF_ERROR(GetDescendants(
          slices, tables::SliceTable::Id(start_id_uint), descendants));
      return ExtendWithStartId<tables::DescendantSliceTable>(
          start_id_uint, slices, std::move(descendants));
    }
    case Type::kSliceByStack: {
      Query q;
      q.constraints = {slices.stack_id().eq(start_id)};
      for (auto it = slices.FilterToIterator(q); it; ++it) {
        RETURN_IF_ERROR(GetDescendants(slices, it.id(), descendants));
      }
      return ExtendWithStartId<tables::DescendantSliceByStackTable>(
          start_id, slices, std::move(descendants));
    }
  }
  PERFETTO_FATAL("For GCC");
}

Table::Schema Descendant::CreateSchema() {
  switch (type_) {
    case Type::kSlice:
      return tables::DescendantSliceTable::ComputeStaticSchema();
    case Type::kSliceByStack:
      return tables::DescendantSliceByStackTable::ComputeStaticSchema();
  }
  PERFETTO_FATAL("For GCC");
}

std::string Descendant::TableName() {
  switch (type_) {
    case Type::kSlice:
      return tables::DescendantSliceTable::Name();
    case Type::kSliceByStack:
      return tables::DescendantSliceByStackTable::Name();
  }
  PERFETTO_FATAL("For GCC");
}

uint32_t Descendant::EstimateRowCount() {
  return 1;
}

// static
std::optional<std::vector<tables::SliceTable::RowNumber>>
Descendant::GetDescendantSlices(const tables::SliceTable& slices,
                                SliceId slice_id) {
  std::vector<tables::SliceTable::RowNumber> ret;
  auto status = GetDescendants(slices, slice_id, ret);
  if (!status.ok())
    return std::nullopt;
  return std::move(ret);
}

}  // namespace perfetto::trace_processor
