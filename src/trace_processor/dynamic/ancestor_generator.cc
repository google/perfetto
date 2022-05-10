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

#include "src/trace_processor/dynamic/ancestor_generator.h"

#include <memory>
#include <set>

#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {
namespace {
uint32_t GetConstraintColumnIndex(AncestorGenerator::Ancestor type,
                                  TraceProcessorContext* context) {
  switch (type) {
    case AncestorGenerator::Ancestor::kSlice:
      return context->storage->slice_table().GetColumnCount();
    case AncestorGenerator::Ancestor::kStackProfileCallsite:
      return context->storage->stack_profile_callsite_table().GetColumnCount();
    case AncestorGenerator::Ancestor::kSliceByStack:
      return context->storage->slice_table().GetColumnCount();
  }
  return 0;
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

template <typename T>
base::Status BuildAncestorsRowMap(const T& table,
                                  typename T::Id starting_id,
                                  RowMap& rowmap_return) {
  auto start_row = table.id().IndexOf(starting_id);
  if (!start_row) {
    return base::ErrStatus("no row with id %" PRIu32 "",
                           static_cast<uint32_t>(starting_id.value));
  }

  std::vector<uint32_t> parent_rows;
  auto maybe_parent_id = table.parent_id()[*start_row];
  while (maybe_parent_id) {
    uint32_t parent_row = table.id().IndexOf(*maybe_parent_id).value();
    parent_rows.push_back(parent_row);
    // Update the loop variable by looking up the next parent_id.
    maybe_parent_id = table.parent_id()[parent_row];
  }
  rowmap_return = RowMap{parent_rows};
  return base::OkStatus();
}

// Constraint_value is used to construct the hidden column "start_id"
// needed by SQL.
// Starting_id refers to the id that is used to generate the ancestors.
template <typename T>
base::Status BuildAncestorsTable(int64_t constraint_value,
                                 const T& table,
                                 typename T::Id starting_id,
                                 std::unique_ptr<Table>& table_return) {
  // Build up all the parents row ids.
  RowMap ancestors;
  RETURN_IF_ERROR(BuildAncestorsRowMap(table, starting_id, ancestors));

  table_return.reset(new Table(ExtendTableWithStartId(
      table.Apply(std::move(ancestors)), constraint_value)));
  return base::OkStatus();
}
}  // namespace

AncestorGenerator::AncestorGenerator(Ancestor type,
                                     TraceProcessorContext* context)
    : type_(type), context_(context) {}

base::Status AncestorGenerator::ValidateConstraints(
    const QueryConstraints& qc) {
  const auto& cs = qc.constraints();

  int column = static_cast<int>(GetConstraintColumnIndex(type_, context_));
  auto id_fn = [column](const QueryConstraints::Constraint& c) {
    return c.column == column && sqlite_utils::IsOpEq(c.op);
  };
  bool has_id_cs = std::find_if(cs.begin(), cs.end(), id_fn) != cs.end();
  return has_id_cs ? base::OkStatus()
                   : base::ErrStatus("Failed to find required constraints");
}

base::Status AncestorGenerator::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>&,
    const BitVector&,
    std::unique_ptr<Table>& table_return) {
  uint32_t column = GetConstraintColumnIndex(type_, context_);
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
    case Ancestor::kSlice: {
      RETURN_IF_ERROR(BuildAncestorsTable(
          /* constraint_id = */ start_id, context_->storage->slice_table(),
          /* starting_id = */ SliceId(static_cast<uint32_t>(start_id)),
          table_return));
      return base::OkStatus();
    }

    case Ancestor::kStackProfileCallsite: {
      RETURN_IF_ERROR(BuildAncestorsTable(
          /* constraint_id = */ start_id,
          context_->storage->stack_profile_callsite_table(),
          /* starting_id = */ CallsiteId(static_cast<uint32_t>(start_id)),
          table_return));
      return base::OkStatus();
    }

    case Ancestor::kSliceByStack: {
      // Find the all slice ids that have the stack id and find all the
      // ancestors of the slice ids.
      const auto& slice_table = context_->storage->slice_table();

      auto result = RowMap();
      auto slice_ids =
          slice_table.FilterToRowMap({slice_table.stack_id().eq(start_id)});

      for (auto id_it = slice_ids.IterateRows(); id_it; id_it.Next()) {
        auto slice_id = slice_table.id()[id_it.index()];

        auto ancestors = GetAncestorSlices(slice_table, slice_id);
        for (auto row_it = ancestors->IterateRows(); row_it; row_it.Next()) {
          result.Insert(row_it.index());
        }
      }

      table_return.reset(new Table(ExtendTableWithStartId(
          slice_table.Apply(std::move(result)), start_id)));
      return base::OkStatus();
    }
  }
  return base::ErrStatus("unknown AncestorGenerator type");
}

Table::Schema AncestorGenerator::CreateSchema() {
  Table::Schema final_schema;
  switch (type_) {
    case Ancestor::kSlice:
      final_schema = tables::SliceTable::Schema();
      break;
    case Ancestor::kStackProfileCallsite:
      final_schema = tables::StackProfileCallsiteTable::Schema();
      break;
    case Ancestor::kSliceByStack:
      final_schema = tables::SliceTable::Schema();
      break;
  }
  final_schema.columns.push_back(Table::Schema::Column{
      "start_id", SqlValue::Type::kLong, /* is_id = */ false,
      /* is_sorted = */ false, /* is_hidden = */ true, /* is_set_id */ false});
  return final_schema;
}

std::string AncestorGenerator::TableName() {
  switch (type_) {
    case Ancestor::kSlice:
      return "ancestor_slice";
    case Ancestor::kStackProfileCallsite:
      return "experimental_ancestor_stack_profile_callsite";
    case Ancestor::kSliceByStack:
      return "ancestor_slice_by_stack";
  }
  return "ancestor_unknown";
}

uint32_t AncestorGenerator::EstimateRowCount() {
  return 1;
}

// static
base::Optional<RowMap> AncestorGenerator::GetAncestorSlices(
    const tables::SliceTable& slices,
    SliceId slice_id) {
  RowMap ret;
  auto status = BuildAncestorsRowMap(slices, slice_id, ret);
  if (!status.ok())
    return base::nullopt;
  return std::move(ret);  // -Wreturn-std-move-in-c++11
}

}  // namespace trace_processor
}  // namespace perfetto
