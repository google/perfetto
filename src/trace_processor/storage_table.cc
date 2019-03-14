/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/storage_table.h"

namespace perfetto {
namespace trace_processor {

StorageTable::StorageTable() = default;
StorageTable::~StorageTable() = default;

base::Optional<Table::Schema> StorageTable::Init(int, const char* const*) {
  schema_ = CreateStorageSchema();
  return schema_.ToTableSchema();
}

std::unique_ptr<Table::Cursor> StorageTable::CreateCursor(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  auto iterator = CreateBestRowIterator(qc, argv);
  if (!iterator)
    return nullptr;
  return std::unique_ptr<Cursor>(
      new Cursor(std::move(iterator), schema_.mutable_columns()));
}

std::unique_ptr<RowIterator> StorageTable::CreateBestRowIterator(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  const auto& cs = qc.constraints();
  auto obs = RemoveRedundantOrderBy(cs, qc.order_by());

  // Figure out whether the data is already ordered and which order we should
  // traverse the data.
  bool is_ordered, is_desc = false;
  std::tie(is_ordered, is_desc) = IsOrdered(obs);

  // Create the range iterator and if we are sorted, just return it.
  auto index = CreateRangeIterator(cs, argv);
  if (!index.error().empty()) {
    SetErrorMessage(sqlite3_mprintf(index.error().c_str()));
    return nullptr;
  }

  if (is_ordered)
    return index.ToRowIterator(is_desc);

  // Otherwise, create the sorted vector of indices and create the vector
  // iterator.
  return std::unique_ptr<VectorRowIterator>(
      new VectorRowIterator(CreateSortedIndexVector(std::move(index), obs)));
}

FilteredRowIndex StorageTable::CreateRangeIterator(
    const std::vector<QueryConstraints::Constraint>& cs,
    sqlite3_value** argv) {
  // Try and bound the search space to the smallest possible index region and
  // store any leftover constraints to filter using bitvector.
  uint32_t min_idx = 0;
  uint32_t max_idx = RowCount();
  std::vector<size_t> bitvector_cs;
  for (size_t i = 0; i < cs.size(); i++) {
    const auto& c = cs[i];
    size_t column = static_cast<size_t>(c.iColumn);
    auto bounds = schema_.GetColumn(column).BoundFilter(c.op, argv[i]);

    min_idx = std::max(min_idx, bounds.min_idx);
    max_idx = std::min(max_idx, bounds.max_idx);

    // If the lower bound is higher than the upper bound, return a zero-sized
    // range iterator.
    if (min_idx >= max_idx)
      return FilteredRowIndex(min_idx, min_idx);

    if (!bounds.consumed)
      bitvector_cs.emplace_back(i);
  }

  // Create an filter index and allow each of the columns filter on it.
  FilteredRowIndex index(min_idx, max_idx);
  for (const auto& c_idx : bitvector_cs) {
    const auto& c = cs[c_idx];
    auto* value = argv[c_idx];

    const auto& schema_col = schema_.GetColumn(static_cast<size_t>(c.iColumn));
    schema_col.Filter(c.op, value, &index);

    if (!index.error().empty())
      break;
  }
  return index;
}

std::pair<bool, bool> StorageTable::IsOrdered(
    const std::vector<QueryConstraints::OrderBy>& obs) {
  if (obs.size() == 0)
    return std::make_pair(true, false);

  if (obs.size() != 1)
    return std::make_pair(false, false);

  const auto& ob = obs[0];
  auto col = static_cast<size_t>(ob.iColumn);
  return std::make_pair(schema_.GetColumn(col).IsNaturallyOrdered(), ob.desc);
}

std::vector<QueryConstraints::OrderBy> StorageTable::RemoveRedundantOrderBy(
    const std::vector<QueryConstraints::Constraint>& cs,
    const std::vector<QueryConstraints::OrderBy>& obs) {
  std::vector<QueryConstraints::OrderBy> filtered;
  std::set<int> equality_cols;
  for (const auto& c : cs) {
    if (sqlite_utils::IsOpEq(c.op))
      equality_cols.emplace(c.iColumn);
  }
  for (const auto& o : obs) {
    if (equality_cols.count(o.iColumn) > 0)
      continue;
    filtered.emplace_back(o);
  }
  return filtered;
}

std::vector<uint32_t> StorageTable::CreateSortedIndexVector(
    FilteredRowIndex index,
    const std::vector<QueryConstraints::OrderBy>& obs) {
  PERFETTO_DCHECK(obs.size() > 0);

  // Retrieve the index created above from the index.
  std::vector<uint32_t> sorted_rows = index.ToRowVector();

  std::vector<StorageColumn::Comparator> comparators;
  for (const auto& ob : obs) {
    auto col = static_cast<size_t>(ob.iColumn);
    comparators.emplace_back(schema_.GetColumn(col).Sort(ob));
  }

  auto comparator = [&comparators](uint32_t f, uint32_t s) {
    for (const auto& comp : comparators) {
      int c = comp(f, s);
      if (c != 0)
        return c < 0;
    }
    return false;
  };
  std::sort(sorted_rows.begin(), sorted_rows.end(), comparator);

  return sorted_rows;
}

bool StorageTable::HasEqConstraint(const QueryConstraints& qc,
                                   const std::string& col_name) {
  size_t c_idx = schema().ColumnIndexFromName(col_name);
  auto fn = [c_idx](const QueryConstraints::Constraint& c) {
    return c.iColumn == static_cast<int>(c_idx) && sqlite_utils::IsOpEq(c.op);
  };
  const auto& cs = qc.constraints();
  return std::find_if(cs.begin(), cs.end(), fn) != cs.end();
}

StorageTable::Cursor::Cursor(std::unique_ptr<RowIterator> iterator,
                             std::vector<std::unique_ptr<StorageColumn>>* cols)
    : iterator_(std::move(iterator)), columns_(std::move(cols)) {}

int StorageTable::Cursor::Next() {
  iterator_->NextRow();
  return SQLITE_OK;
}

int StorageTable::Cursor::Eof() {
  return iterator_->IsEnd();
}

int StorageTable::Cursor::Column(sqlite3_context* context, int raw_col) {
  size_t column = static_cast<size_t>(raw_col);
  (*columns_)[column]->ReportResult(context, iterator_->Row());
  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
