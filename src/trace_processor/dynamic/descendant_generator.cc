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
namespace {
uint32_t GetConstraintColumnIndex(TraceProcessorContext* context) {
  return context->storage->slice_table().GetColumnCount();
}

template <typename T>
Table ExtendTableWithStartId(const T& table, int64_t constraint_value) {
  // Add a new column that includes the constraint.
  std::unique_ptr<NullableVector<int64_t>> child_ids(
      new NullableVector<int64_t>());
  for (uint32_t i = 0; i < table.row_count(); ++i)
    child_ids->Append(constraint_value);
  return table.ExtendWithColumn(
      "start_id", std::move(child_ids),
      TypedColumn<uint32_t>::default_flags() | TypedColumn<uint32_t>::kHidden);
}

base::Status BuildDescendantsRowMap(const tables::SliceTable& slices,
                                    SliceId starting_id,
                                    RowMap& rowmap_return) {
  auto start_row = slices.id().IndexOf(starting_id);
  // The query gave an invalid ID that doesn't exist in the slice table.
  if (!start_row) {
    return base::ErrStatus("no row with id %" PRIu32 "",
                           static_cast<uint32_t>(starting_id.value));
  }

  // All nested descendents must be on the same track, with a ts between
  // |start_id.ts| and |start_id.ts| + |start_id.dur|, and who's depth is larger
  // then |start_row|'s. So we just use Filter to select all relevant slices.
  rowmap_return = slices.FilterToRowMap(
      {slices.ts().ge(slices.ts()[*start_row]),
       slices.ts().le(slices.ts()[*start_row] + slices.dur()[*start_row]),
       slices.track_id().eq(slices.track_id()[*start_row].value),
       slices.depth().gt(slices.depth()[*start_row])});
  return base::OkStatus();
}

base::Status BuildDescendantsTable(int64_t constraint_value,
                                   const tables::SliceTable& slices,
                                   SliceId starting_id,
                                   std::unique_ptr<Table>& table_return) {
  // Build up all the children row ids.
  RowMap descendants;
  RETURN_IF_ERROR(BuildDescendantsRowMap(slices, starting_id, descendants));

  table_return.reset(new Table(ExtendTableWithStartId(
      slices.Apply(std::move(descendants)), constraint_value)));
  return base::OkStatus();
}
}  // namespace

DescendantGenerator::DescendantGenerator(Descendant type,
                                         TraceProcessorContext* context)
    : type_(type), context_(context) {}

base::Status DescendantGenerator::ValidateConstraints(
    const QueryConstraints& qc) {
  const auto& cs = qc.constraints();

  int column = static_cast<int>(GetConstraintColumnIndex(context_));
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

  uint32_t column = GetConstraintColumnIndex(context_);
  auto constraint_it =
      std::find_if(cs.begin(), cs.end(), [column](const Constraint& c) {
        return c.col_idx == column && c.op == FilterOp::kEq;
      });
  PERFETTO_DCHECK(constraint_it != cs.end());
  if (constraint_it == cs.end() ||
      constraint_it->value.type != SqlValue::Type::kLong) {
    return base::ErrStatus("invalid start_id");
  }
  auto start_id = constraint_it->value.AsLong();

  switch (type_) {
    case Descendant::kSlice: {
      RETURN_IF_ERROR(BuildDescendantsTable(
          start_id, slices, SliceId(static_cast<uint32_t>(start_id)),
          table_return));
      return base::OkStatus();
    }

    case Descendant::kSliceByStack: {
      auto result = RowMap();
      auto slice_ids = slices.FilterToRowMap({slices.stack_id().eq(start_id)});

      for (auto id_it = slice_ids.IterateRows(); id_it; id_it.Next()) {
        auto slice_id = slices.id()[id_it.index()];

        auto descendants = GetDescendantSlices(slices, slice_id);
        for (auto row_it = descendants->IterateRows(); row_it; row_it.Next()) {
          result.Insert(row_it.index());
        }
      }

      table_return.reset(new Table(
          ExtendTableWithStartId(slices.Apply(std::move(result)), start_id)));
      return base::OkStatus();
    }
  }
  return base::ErrStatus("unknown DescendantGenerator type");
}

Table::Schema DescendantGenerator::CreateSchema() {
  auto schema = tables::SliceTable::Schema();
  schema.columns.push_back(Table::Schema::Column{
      "start_id", SqlValue::Type::kLong, /* is_id = */ false,
      /* is_sorted = */ false, /* is_hidden = */ true,
      /* is_set_id = */ false});
  return schema;
}

std::string DescendantGenerator::TableName() {
  switch (type_) {
    case Descendant::kSlice:
      return "descendant_slice";
    case Descendant::kSliceByStack:
      return "descendant_slice_by_stack";
  }
  return "descendant_unknown";
}

uint32_t DescendantGenerator::EstimateRowCount() {
  return 1;
}

// static
base::Optional<RowMap> DescendantGenerator::GetDescendantSlices(
    const tables::SliceTable& slices,
    SliceId slice_id) {
  RowMap ret;
  auto status = BuildDescendantsRowMap(slices, slice_id, ret);
  if (!status.ok())
    return base::nullopt;
  return std::move(ret);  // -Wreturn-std-move-in-c++11
}

}  // namespace trace_processor
}  // namespace perfetto
