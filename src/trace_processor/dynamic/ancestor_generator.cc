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

#include "src/trace_processor/types/trace_processor_context.h"

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
  }
  return 0;
}

template <typename T>
base::Optional<RowMap> BuildAncestorsRowMap(const T& table,
                                            typename T::Id starting_id) {
  auto start_row = table.id().IndexOf(starting_id);

  if (!start_row) {
    // TODO(lalitm): Ideally this should result in an error, or be filtered out
    // during ValidateConstraints so we can just dereference |start_row|
    // directly. However ValidateConstraints doesn't know the value we're
    // filtering for so can't ensure it exists. For now we return a nullptr
    // which will cause the query to surface an error with the message "SQL
    // error: constraint failed".
    return base::nullopt;
  }

  std::vector<uint32_t> ids;
  auto maybe_parent_id = table.parent_id()[*start_row];
  while (maybe_parent_id) {
    ids.push_back(maybe_parent_id.value().value);
    // Update the loop variable by looking up the next parent_id.
    maybe_parent_id = table.parent_id()[*table.id().IndexOf(*maybe_parent_id)];
  }
  return RowMap(std::move(ids));
}

template <typename T>
std::unique_ptr<Table> BuildAncestorsTable(const T& table,
                                           typename T::Id starting_id) {
  // Build up all the parents row ids.
  auto ancestors = BuildAncestorsRowMap(table, starting_id);
  if (!ancestors) {
    return nullptr;
  }
  // Add a new column that includes the constraint.
  std::unique_ptr<NullableVector<uint32_t>> child_ids(
      new NullableVector<uint32_t>());
  for (uint32_t i = 0; i < ancestors->size(); ++i)
    child_ids->Append(starting_id.value);
  return std::unique_ptr<Table>(
      new Table(table.Apply(std::move(*ancestors))
                    .ExtendWithColumn("start_id", std::move(child_ids),
                                      TypedColumn<uint32_t>::default_flags() |
                                          TypedColumn<uint32_t>::kHidden)));
}
}  // namespace

AncestorGenerator::AncestorGenerator(Ancestor type,
                                     TraceProcessorContext* context)
    : type_(type), context_(context) {}

util::Status AncestorGenerator::ValidateConstraints(
    const QueryConstraints& qc) {
  const auto& cs = qc.constraints();

  int column = static_cast<int>(GetConstraintColumnIndex(type_, context_));
  auto id_fn = [column](const QueryConstraints::Constraint& c) {
    return c.column == column && c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_id_cs = std::find_if(cs.begin(), cs.end(), id_fn) != cs.end();
  return has_id_cs ? util::OkStatus()
                   : util::ErrStatus("Failed to find required constraints");
}

std::unique_ptr<Table> AncestorGenerator::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>&) {
  uint32_t column = GetConstraintColumnIndex(type_, context_);
  auto it = std::find_if(cs.begin(), cs.end(), [column](const Constraint& c) {
    return c.col_idx == column && c.op == FilterOp::kEq;
  });
  PERFETTO_DCHECK(it != cs.end());

  auto start_id = static_cast<uint32_t>(it->value.AsLong());
  switch (type_) {
    case Ancestor::kSlice:
      return BuildAncestorsTable(context_->storage->slice_table(),
                                 SliceId(start_id));
    case Ancestor::kStackProfileCallsite:
      return BuildAncestorsTable(
          context_->storage->stack_profile_callsite_table(),
          CallsiteId(start_id));
  }
  return nullptr;
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
  }
  final_schema.columns.push_back(Table::Schema::Column{
      "start_id", SqlValue::Type::kLong, /* is_id = */ false,
      /* is_sorted = */ false, /* is_hidden = */ true});
  return final_schema;
}

std::string AncestorGenerator::TableName() {
  switch (type_) {
    case Ancestor::kSlice:
      return "ancestor_slice";
    case Ancestor::kStackProfileCallsite:
      return "experimental_ancestor_stack_profile_callsite";
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
  return BuildAncestorsRowMap(slices, slice_id);
}

}  // namespace trace_processor
}  // namespace perfetto
