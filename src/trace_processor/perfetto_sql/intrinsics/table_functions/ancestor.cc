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

#include <memory>
#include <set>

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/tables_py.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

AncestorSliceTable::~AncestorSliceTable() = default;
AncestorStackProfileCallsiteTable::~AncestorStackProfileCallsiteTable() =
    default;
AncestorSliceByStackTable::~AncestorSliceByStackTable() = default;

}  // namespace tables

namespace {

uint32_t GetConstraintColumnIndex(Ancestor::Type type) {
  switch (type) {
    case Ancestor::Type::kSlice:
      return tables::AncestorSliceTable::ColumnIndex::start_id;
    case Ancestor::Type::kStackProfileCallsite:
      return tables::AncestorStackProfileCallsiteTable::ColumnIndex::start_id;
    case Ancestor::Type::kSliceByStack:
      return tables::AncestorSliceByStackTable::ColumnIndex::start_stack_id;
  }
  PERFETTO_FATAL("For GCC");
}

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
base::Status BuildAncestorsTable(typename ParentTable::Id id,
                                 const ParentTable& table,
                                 std::unique_ptr<Table>& table_return) {
  // Build up all the parents row ids.
  std::vector<typename ParentTable::RowNumber> ancestors;
  RETURN_IF_ERROR(GetAncestors(table, id, ancestors));
  table_return =
      ExtendWithStartId<ChildTable>(id.value, table, std::move(ancestors));
  return base::OkStatus();
}

}  // namespace

Ancestor::Ancestor(Type type, const TraceStorage* storage)
    : type_(type), storage_(storage) {}

base::Status Ancestor::ValidateConstraints(const QueryConstraints& qc) {
  const auto& cs = qc.constraints();

  int column = static_cast<int>(GetConstraintColumnIndex(type_));
  auto id_fn = [column](const QueryConstraints::Constraint& c) {
    return c.column == column && sqlite_utils::IsOpEq(c.op);
  };
  bool has_id_cs = std::find_if(cs.begin(), cs.end(), id_fn) != cs.end();
  return has_id_cs ? base::OkStatus()
                   : base::ErrStatus("Failed to find required constraints");
}

base::Status Ancestor::ComputeTable(const std::vector<Constraint>& cs,
                                    const std::vector<Order>&,
                                    const BitVector&,
                                    std::unique_ptr<Table>& table_return) {
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
        table_return = tables::AncestorSliceTable::SelectAndExtendParent(
            storage_->slice_table(), {}, {});
        break;
      case Type::kStackProfileCallsite:
        table_return =
            tables::AncestorStackProfileCallsiteTable::SelectAndExtendParent(
                storage_->stack_profile_callsite_table(), {}, {});
        break;
      case Type::kSliceByStack:
        table_return = tables::AncestorSliceByStackTable::SelectAndExtendParent(
            storage_->slice_table(), {}, {});
        break;
    }
    return base::OkStatus();
  }
  if (constraint_it->value.type != SqlValue::Type::kLong) {
    return base::ErrStatus("start id should be an integer.");
  }

  int64_t start_id = constraint_it->value.AsLong();
  uint32_t start_id_uint = static_cast<uint32_t>(start_id);
  switch (type_) {
    case Type::kSlice:
      return BuildAncestorsTable<tables::AncestorSliceTable>(
          SliceId(start_id_uint), storage_->slice_table(), table_return);

    case Type::kStackProfileCallsite:
      return BuildAncestorsTable<tables::AncestorStackProfileCallsiteTable>(
          CallsiteId(start_id_uint), storage_->stack_profile_callsite_table(),
          table_return);

    case Type::kSliceByStack: {
      // Find the all slice ids that have the stack id and find all the
      // ancestors of the slice ids.
      const auto& slice_table = storage_->slice_table();
      auto it =
          slice_table.FilterToIterator({slice_table.stack_id().eq(start_id)});
      std::vector<tables::SliceTable::RowNumber> ancestors;
      for (; it; ++it) {
        RETURN_IF_ERROR(GetAncestors(slice_table, it.id(), ancestors));
      }
      // Sort to keep the slices in timestamp order.
      std::sort(ancestors.begin(), ancestors.end());
      table_return = ExtendWithStartId<tables::AncestorSliceByStackTable>(
          start_id, slice_table, std::move(ancestors));
      return base::OkStatus();
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

}  // namespace trace_processor
}  // namespace perfetto
