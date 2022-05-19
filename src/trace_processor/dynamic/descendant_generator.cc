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

#include "src/trace_processor/dynamic/descendant_generator.h"

#include <memory>
#include <set>

#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

#define PERFETTO_TP_DESCENDANT_SLICE_TABLE_DEF(NAME, PARENT, C) \
  NAME(DescendantSliceTable, "descendant_slice")                \
  PARENT(PERFETTO_TP_SLICE_TABLE_DEF, C)                        \
  C(uint32_t, start_id, Column::Flag::kHidden)

PERFETTO_TP_TABLE(PERFETTO_TP_DESCENDANT_SLICE_TABLE_DEF);

#define PERFETTO_TP_DESCENDANT_SLICE_BY_STACK_TABLE_DEF(NAME, PARENT, C) \
  NAME(DescendantSliceByStackTable, "descendant_slice_by_stack")         \
  PARENT(PERFETTO_TP_SLICE_TABLE_DEF, C)                                 \
  C(int64_t, start_id, Column::Flag::kHidden)

PERFETTO_TP_TABLE(PERFETTO_TP_DESCENDANT_SLICE_BY_STACK_TABLE_DEF);

DescendantSliceTable::~DescendantSliceTable() = default;
DescendantSliceByStackTable::~DescendantSliceByStackTable() = default;

}  // namespace tables

namespace {

template <typename ChildTable, typename ParentTable, typename ConstraintType>
std::unique_ptr<Table> ExtendWithStartId(
    ConstraintType constraint_id,
    const ParentTable& table,
    std::vector<typename ParentTable::RowNumber> parent_rows) {
  NullableVector<ConstraintType> start_ids;
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

  // All nested descendents must be on the same track, with a ts between
  // |start_id.ts| and |start_id.ts| + |start_id.dur|, and who's depth is larger
  // then |start_row|'s. So we just use Filter to select all relevant slices.
  auto cs = {slices.ts().ge(start_ref->ts()),
             slices.ts().le(start_ref->ts() + start_ref->dur()),
             slices.track_id().eq(start_ref->track_id().value),
             slices.depth().gt(start_ref->depth())};

  // It's important we insert directly into |row_numbers_accumulator| and not
  // overwrite it because we expect the existing elements in
  // |row_numbers_accumulator| to be preserved.
  for (auto it = slices.FilterToIterator(cs); it; ++it) {
    row_numbers_accumulator.emplace_back(it.row_number());
  }
  return base::OkStatus();
}

}  // namespace

DescendantGenerator::DescendantGenerator(Descendant type,
                                         TraceProcessorContext* context)
    : type_(type), context_(context) {}

base::Status DescendantGenerator::ValidateConstraints(
    const QueryConstraints& qc) {
  const auto& cs = qc.constraints();

  int column =
      static_cast<int>(tables::DescendantSliceTable::ColumnIndex::start_id);
  auto id_fn = [column](const QueryConstraints::Constraint& c) {
    return c.column == column && sqlite_utils::IsOpEq(c.op);
  };
  bool has_id_cs = std::find_if(cs.begin(), cs.end(), id_fn) != cs.end();
  return has_id_cs ? base::OkStatus()
                   : base::ErrStatus("Failed to find required constraints");
}

base::Status DescendantGenerator::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>&,
    const BitVector&,
    std::unique_ptr<Table>& table_return) {
  const auto& slices = context_->storage->slice_table();

  uint32_t column = tables::DescendantSliceTable::ColumnIndex::start_id;
  auto constraint_it =
      std::find_if(cs.begin(), cs.end(), [column](const Constraint& c) {
        return c.col_idx == column && c.op == FilterOp::kEq;
      });
  PERFETTO_DCHECK(constraint_it != cs.end());
  if (constraint_it == cs.end() ||
      constraint_it->value.type != SqlValue::Type::kLong) {
    return base::ErrStatus("invalid start_id");
  }

  int64_t start_id = constraint_it->value.AsLong();
  std::vector<tables::SliceTable::RowNumber> descendants;
  switch (type_) {
    case Descendant::kSlice: {
      // Build up all the children row ids.
      uint32_t start_id_uint = static_cast<uint32_t>(start_id);
      RETURN_IF_ERROR(GetDescendants(
          slices, tables::SliceTable::Id(start_id_uint), descendants));
      table_return = ExtendWithStartId<tables::DescendantSliceTable>(
          start_id_uint, slices, std::move(descendants));
      break;
    }
    case Descendant::kSliceByStack: {
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

Table::Schema DescendantGenerator::CreateSchema() {
  switch (type_) {
    case Descendant::kSlice:
      return tables::DescendantSliceTable::Schema();
    case Descendant::kSliceByStack:
      return tables::DescendantSliceByStackTable::Schema();
  }
  PERFETTO_FATAL("For GCC");
}

std::string DescendantGenerator::TableName() {
  switch (type_) {
    case Descendant::kSlice:
      return tables::DescendantSliceTable::Name();
    case Descendant::kSliceByStack:
      return tables::DescendantSliceByStackTable::Name();
  }
  PERFETTO_FATAL("For GCC");
}

uint32_t DescendantGenerator::EstimateRowCount() {
  return 1;
}

// static
base::Optional<std::vector<tables::SliceTable::RowNumber>>
DescendantGenerator::GetDescendantSlices(const tables::SliceTable& slices,
                                         SliceId slice_id) {
  std::vector<tables::SliceTable::RowNumber> ret;
  auto status = GetDescendants(slices, slice_id, ret);
  if (!status.ok())
    return base::nullopt;
  return std::move(ret);
}

}  // namespace trace_processor
}  // namespace perfetto
