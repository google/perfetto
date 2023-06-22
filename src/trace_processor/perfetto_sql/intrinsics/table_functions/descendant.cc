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

#include <memory>
#include <set>

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/tables_py.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {
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
  std::vector<Constraint> cs;
  if (start_ref->dur() >= 0) {
    cs.emplace_back(slices.ts().le(start_ref->ts() + start_ref->dur()));
  }

  // All nested descendents must be on the same track, with a ts greater than
  // |start_ref.ts| and whose depth is larger than |start_ref|'s.
  cs.emplace_back(slices.ts().ge(start_ref->ts()));
  cs.emplace_back(slices.track_id().eq(start_ref->track_id().value));
  cs.emplace_back(slices.depth().gt(start_ref->depth()));

  // It's important we insert directly into |row_numbers_accumulator| and not
  // overwrite it because we expect the existing elements in
  // |row_numbers_accumulator| to be preserved.
  for (auto it = slices.FilterToIterator(cs); it; ++it) {
    row_numbers_accumulator.emplace_back(it.row_number());
  }
  return base::OkStatus();
}

uint32_t GetConstraintColumnIndex(Descendant::Type type) {
  switch (type) {
    case Descendant::Type::kSlice:
      return tables::DescendantSliceTable::ColumnIndex::start_id;
    case Descendant::Type::kSliceByStack:
      return tables::DescendantSliceByStackTable::ColumnIndex::start_stack_id;
  }
  PERFETTO_FATAL("For GCC");
}

}  // namespace

Descendant::Descendant(Type type, const TraceStorage* storage)
    : type_(type), storage_(storage) {}

base::Status Descendant::ValidateConstraints(const QueryConstraints& qc) {
  const auto& cs = qc.constraints();

  int column = static_cast<int>(GetConstraintColumnIndex(type_));
  auto id_fn = [column](const QueryConstraints::Constraint& c) {
    return c.column == column && sqlite_utils::IsOpEq(c.op);
  };
  bool has_id_cs = std::find_if(cs.begin(), cs.end(), id_fn) != cs.end();
  return has_id_cs ? base::OkStatus()
                   : base::ErrStatus("Failed to find required constraints");
}

base::Status Descendant::ComputeTable(const std::vector<Constraint>& cs,
                                      const std::vector<Order>&,
                                      const BitVector&,
                                      std::unique_ptr<Table>& table_return) {
  const auto& slices = storage_->slice_table();

  uint32_t column = GetConstraintColumnIndex(type_);
  auto constraint_it =
      std::find_if(cs.begin(), cs.end(), [column](const Constraint& c) {
        return c.col_idx == column && c.op == FilterOp::kEq;
      });
  if (constraint_it == cs.end()) {
    return base::ErrStatus("no start id specified.");
  }
  if (constraint_it->value.type == SqlValue::Type::kNull) {
    // Nothing matches a null id so return an empty table.
    switch (type_) {
      case Type::kSlice:
        table_return = tables::DescendantSliceTable::SelectAndExtendParent(
            storage_->slice_table(), {}, {});
        break;
      case Type::kSliceByStack:
        table_return =
            tables::DescendantSliceByStackTable::SelectAndExtendParent(
                storage_->slice_table(), {}, {});
        break;
    }
    return base::OkStatus();
  }
  if (constraint_it->value.type != SqlValue::Type::kLong) {
    return base::ErrStatus("start id should be an integer.");
  }

  int64_t start_id = constraint_it->value.AsLong();
  std::vector<tables::SliceTable::RowNumber> descendants;
  switch (type_) {
    case Type::kSlice: {
      // Build up all the children row ids.
      uint32_t start_id_uint = static_cast<uint32_t>(start_id);
      RETURN_IF_ERROR(GetDescendants(
          slices, tables::SliceTable::Id(start_id_uint), descendants));
      table_return = ExtendWithStartId<tables::DescendantSliceTable>(
          start_id_uint, slices, std::move(descendants));
      break;
    }
    case Type::kSliceByStack: {
      auto sbs_cs = {slices.stack_id().eq(start_id)};
      for (auto it = slices.FilterToIterator(sbs_cs); it; ++it) {
        RETURN_IF_ERROR(GetDescendants(slices, it.id(), descendants));
      }
      table_return = ExtendWithStartId<tables::DescendantSliceByStackTable>(
          start_id, slices, std::move(descendants));
      break;
    }
  }

  return base::OkStatus();
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

}  // namespace trace_processor
}  // namespace perfetto
