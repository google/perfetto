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

#include "src/trace_processor/dynamic/ancestor_slice_generator.h"

#include <memory>
#include <set>

#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

AncestorSliceGenerator::AncestorSliceGenerator(TraceProcessorContext* context)
    : context_(context) {}

AncestorSliceGenerator::~AncestorSliceGenerator() = default;

util::Status AncestorSliceGenerator::ValidateConstraints(
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

std::unique_ptr<Table> AncestorSliceGenerator::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>&) {
  using S = tables::SliceTable;

  auto it = std::find_if(cs.begin(), cs.end(), [this](const Constraint& c) {
    return c.col_idx == context_->storage->slice_table().GetColumnCount() &&
           c.op == FilterOp::kEq;
  });
  PERFETTO_DCHECK(it != cs.end());

  const auto& slice = context_->storage->slice_table();
  uint32_t child_id = static_cast<uint32_t>(it->value.AsLong());
  auto start_row = slice.id().IndexOf(S::Id(child_id));

  if (!start_row) {
    // TODO(lalitm): Ideally this should result in an error, or be filtered out
    // during ValidateConstraints so we can just dereference |start_row|
    // directly. However ValidateConstraints doesn't know the value we're
    // filtering for so can't ensure it exists. For now we return a nullptr
    // which will cause the query to surface an error with the message "SQL
    // error: constraint failed".
    return nullptr;
  }

  // Build up all the parents row ids, and a new column that includes the
  // constraint.
  std::vector<uint32_t> ids;
  std::unique_ptr<NullableVector<uint32_t>> child_ids(
      new NullableVector<uint32_t>());

  auto maybe_parent_id = slice.parent_id()[*start_row];
  while (maybe_parent_id) {
    ids.push_back(maybe_parent_id.value().value);
    child_ids->Append(child_id);
    // Update the loop variable by looking up the next parent_id.
    maybe_parent_id = slice.parent_id()[*slice.id().IndexOf(*maybe_parent_id)];
  }
  return std::unique_ptr<Table>(
      new Table(slice.Apply(RowMap(std::move(ids)))
                    .ExtendWithColumn("start_id", std::move(child_ids),
                                      TypedColumn<uint32_t>::default_flags() |
                                          TypedColumn<uint32_t>::kHidden)));
}

Table::Schema AncestorSliceGenerator::CreateSchema() {
  auto schema = tables::SliceTable::Schema();
  schema.columns.push_back(Table::Schema::Column{
      "start_id", SqlValue::Type::kLong, /* is_id = */ false,
      /* is_sorted = */ false, /* is_hidden = */ true});
  return schema;
}

std::string AncestorSliceGenerator::TableName() {
  return "ancestor_slice";
}

uint32_t AncestorSliceGenerator::EstimateRowCount() {
  return 1;
}
}  // namespace trace_processor
}  // namespace perfetto
