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

#include "src/trace_processor/dynamic/descendant_slice_generator.h"

#include <memory>
#include <set>

#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

DescendantSliceGenerator::DescendantSliceGenerator(
    TraceProcessorContext* context)
    : context_(context) {}

DescendantSliceGenerator::~DescendantSliceGenerator() = default;

util::Status DescendantSliceGenerator::ValidateConstraints(
    const QueryConstraints& qc) {
  const auto& cs = qc.constraints();

  auto slice_id_fn = [this](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(
                           context_->storage->slice_table().GetColumnCount()) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_slice_id_cs =
      std::find_if(cs.begin(), cs.end(), slice_id_fn) != cs.end();

  return has_slice_id_cs
             ? util::OkStatus()
             : util::ErrStatus("Failed to find required constraints");
}

std::unique_ptr<Table> DescendantSliceGenerator::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>&) {
  using S = tables::SliceTable;
  const auto& slice = context_->storage->slice_table();

  auto it = std::find_if(cs.begin(), cs.end(), [&slice](const Constraint& c) {
    return c.col_idx == slice.GetColumnCount() && c.op == FilterOp::kEq;
  });
  PERFETTO_DCHECK(it != cs.end());

  uint32_t start_id = static_cast<uint32_t>(it->value.AsLong());
  auto start_row = slice.id().IndexOf(S::Id(start_id));
  // The query gave an invalid ID that doesn't exist in the slice table.
  if (!start_row) {
    // TODO(lalitm): Ideally this should result in an error, or be filtered out
    // during ValidateConstraints so we can just dereference |start_row|
    // directly. However ValidateConstraints doesn't know the value we're
    // filtering for so can't ensure it exists. For now we return a nullptr
    // which will cause the query to surface an error with the message "SQL
    // error: constraint failed".
    return nullptr;
  }

  // All nested descendents must be on the same track, with a ts between
  // |start_id.ts| and |start_id.ts| + |start_id.dur|, and who's depth is larger
  // then |start_row|'s. So we just use Filter to select all relevant slices.
  Table reduced_slice = slice.Filter(
      {slice.ts().ge(slice.ts()[*start_row]),
       slice.ts().le(slice.ts()[*start_row] + slice.dur()[*start_row]),
       slice.track_id().eq(slice.track_id()[*start_row].value),
       slice.depth().gt(slice.depth()[*start_row])});

  // For every row extend it to match the schema, and return it.
  std::unique_ptr<NullableVector<uint32_t>> start_ids(
      new NullableVector<uint32_t>());
  for (size_t i = 0; i < reduced_slice.row_count(); ++i) {
    start_ids->Append(start_id);
  }
  return std::unique_ptr<Table>(
      new Table(std::move(reduced_slice)
                    .ExtendWithColumn("start_id", std::move(start_ids),
                                      TypedColumn<uint32_t>::default_flags() |
                                          TypedColumn<uint32_t>::kHidden)));
}

Table::Schema DescendantSliceGenerator::CreateSchema() {
  auto schema = tables::SliceTable::Schema();
  schema.columns.push_back(Table::Schema::Column{
      "start_id", SqlValue::Type::kLong, /* is_id = */ false,
      /* is_sorted = */ false, /* is_hidden = */ true});
  return schema;
}

std::string DescendantSliceGenerator::TableName() {
  return "descendant_slice";
}

uint32_t DescendantSliceGenerator::EstimateRowCount() {
  return 1;
}
}  // namespace trace_processor
}  // namespace perfetto
