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

#include "src/trace_processor/types/trace_processor_context.h"

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

base::Optional<RowMap> BuildDescendantsRowMap(const tables::SliceTable& slices,
                                              SliceId starting_id) {
  auto start_row = slices.id().IndexOf(starting_id);
  // The query gave an invalid ID that doesn't exist in the slice table.
  if (!start_row) {
    // TODO(lalitm): Ideally this should result in an error, or be filtered out
    // during ValidateConstraints so we can just dereference |start_row|
    // directly. However ValidateConstraints doesn't know the value we're
    // filtering for so can't ensure it exists. For now we return a nullptr
    // which will cause the query to surface an error with the message "SQL
    // error: constraint failed".
    return base::nullopt;
  }

  // All nested descendents must be on the same track, with a ts between
  // |start_id.ts| and |start_id.ts| + |start_id.dur|, and who's depth is larger
  // then |start_row|'s. So we just use Filter to select all relevant slices.
  return slices.FilterToRowMap(
      {slices.ts().ge(slices.ts()[*start_row]),
       slices.ts().le(slices.ts()[*start_row] + slices.dur()[*start_row]),
       slices.track_id().eq(slices.track_id()[*start_row].value),
       slices.depth().gt(slices.depth()[*start_row])});
}

std::unique_ptr<Table> BuildDescendantsTable(int64_t constraint_value,
                                             const tables::SliceTable& slices,
                                             SliceId starting_id) {
  // Build up all the children row ids.
  auto descendants = BuildDescendantsRowMap(slices, starting_id);
  if (!descendants) {
    return nullptr;
  }
  return std::unique_ptr<Table>(new Table(ExtendTableWithStartId(
      slices.Apply(std::move(*descendants)), constraint_value)));
}
}  // namespace

DescendantGenerator::DescendantGenerator(Descendant type,
                                         TraceProcessorContext* context)
    : type_(type), context_(context) {}

util::Status DescendantGenerator::ValidateConstraints(
    const QueryConstraints& qc) {
  const auto& cs = qc.constraints();

  int column = static_cast<int>(GetConstraintColumnIndex(context_));
  auto id_fn = [column](const QueryConstraints::Constraint& c) {
    return c.column == column && c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_id_cs = std::find_if(cs.begin(), cs.end(), id_fn) != cs.end();
  return has_id_cs ? util::OkStatus()
                   : util::ErrStatus("Failed to find required constraints");
}

std::unique_ptr<Table> DescendantGenerator::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>&) {
  const auto& slices = context_->storage->slice_table();

  uint32_t column = GetConstraintColumnIndex(context_);
  auto it = std::find_if(cs.begin(), cs.end(), [column](const Constraint& c) {
    return c.col_idx == column && c.op == FilterOp::kEq;
  });
  PERFETTO_DCHECK(it != cs.end());
  auto start_id = it->value.AsLong();

  switch (type_) {
    case Descendant::kSlice:
      return BuildDescendantsTable(start_id, slices,
                                   SliceId(static_cast<uint32_t>(start_id)));
    case Descendant::kSliceByStack:
      auto result = RowMap();
      auto slice_ids = slices.FilterToRowMap({slices.stack_id().eq(start_id)});

      for (auto id_it = slice_ids.IterateRows(); id_it; id_it.Next()) {
        auto slice_id = slices.id()[id_it.row()];

        auto descendants = GetDescendantSlices(slices, slice_id);
        for (auto row_it = descendants->IterateRows(); row_it; row_it.Next()) {
          result.Insert(row_it.row());
        }
      }

      return std::unique_ptr<Table>(new Table(
          ExtendTableWithStartId(slices.Apply(std::move(result)), start_id)));
  }
  return nullptr;
}

Table::Schema DescendantGenerator::CreateSchema() {
  auto schema = tables::SliceTable::Schema();
  schema.columns.push_back(Table::Schema::Column{
      "start_id", SqlValue::Type::kLong, /* is_id = */ false,
      /* is_sorted = */ false, /* is_hidden = */ true});
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
  return BuildDescendantsRowMap(slices, slice_id);
}

}  // namespace trace_processor
}  // namespace perfetto
