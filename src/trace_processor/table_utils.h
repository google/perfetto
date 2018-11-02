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

#ifndef SRC_TRACE_PROCESSOR_TABLE_UTILS_H_
#define SRC_TRACE_PROCESSOR_TABLE_UTILS_H_

#include <memory>
#include <set>

#include "src/trace_processor/row_iterators.h"
#include "src/trace_processor/storage_schema.h"

namespace perfetto {
namespace trace_processor {
namespace table_utils {

namespace internal {

inline RangeRowIterator CreateRangeIterator(
    const StorageSchema& schema,
    uint32_t size,
    bool desc,
    const std::vector<QueryConstraints::Constraint>& cs,
    sqlite3_value** argv) {
  // Try and bound the search space to the smallest possible index region and
  // store any leftover constraints to filter using bitvector.
  uint32_t min_idx = 0;
  uint32_t max_idx = size;
  std::vector<size_t> bitvector_cs;
  for (size_t i = 0; i < cs.size(); i++) {
    const auto& c = cs[i];
    size_t column = static_cast<size_t>(c.iColumn);
    auto bounds = schema.GetColumn(column).BoundFilter(c.op, argv[i]);
    if (bounds.consumed) {
      min_idx = std::max(min_idx, bounds.min_idx);
      max_idx = std::min(max_idx, bounds.max_idx);
    } else {
      bitvector_cs.emplace_back(i);
    }
  }

  // If we have no other constraints then we can just iterate between min
  // and max.
  if (bitvector_cs.empty())
    return RangeRowIterator(min_idx, max_idx, desc);

  // Otherwise, create a bitvector with true meaning the row should be returned
  // and false otherwise.
  std::vector<bool> filter(max_idx - min_idx, true);
  for (const auto& c_idx : bitvector_cs) {
    const auto& c = cs[c_idx];
    auto* value = argv[c_idx];

    auto col = static_cast<size_t>(c.iColumn);
    auto predicate = schema.GetColumn(col).Filter(c.op, value);

    auto b = filter.begin();
    auto e = filter.end();
    using std::find;
    for (auto it = find(b, e, true); it != e; it = find(it + 1, e, true)) {
      auto filter_idx = static_cast<uint32_t>(std::distance(b, it));
      *it = predicate(min_idx + filter_idx);
    }
  }
  return RangeRowIterator(min_idx, desc, std::move(filter));
}

inline std::pair<bool, bool> IsOrdered(
    const StorageSchema& schema,
    const std::vector<QueryConstraints::OrderBy>& obs) {
  if (obs.size() == 0)
    return std::make_pair(true, false);

  if (obs.size() != 1)
    return std::make_pair(false, false);

  const auto& ob = obs[0];
  auto col = static_cast<size_t>(ob.iColumn);
  return std::make_pair(schema.GetColumn(col).IsNaturallyOrdered(), ob.desc);
}

inline std::vector<QueryConstraints::OrderBy> RemoveRedundantOrderBy(
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

inline std::vector<uint32_t> CreateSortedIndexVector(
    const StorageSchema& schema,
    RangeRowIterator it,
    const std::vector<QueryConstraints::OrderBy>& obs) {
  PERFETTO_DCHECK(obs.size() > 0);

  std::vector<uint32_t> sorted_rows(it.RowCount());
  for (size_t i = 0; !it.IsEnd(); it.NextRow(), i++)
    sorted_rows[i] = it.Row();

  std::vector<StorageSchema::Column::Comparator> comparators;
  for (const auto& ob : obs) {
    auto col = static_cast<size_t>(ob.iColumn);
    comparators.emplace_back(schema.GetColumn(col).Sort(ob));
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

}  // namespace internal

// Creates a row iterator which is optimized for a generic storage schema (i.e.
// it does not make assumptions about values of columns).
inline std::unique_ptr<StorageCursor::RowIterator>
CreateBestRowIteratorForGenericSchema(const StorageSchema& schema,
                                      uint32_t size,
                                      const QueryConstraints& qc,
                                      sqlite3_value** argv) {
  const auto& cs = qc.constraints();
  auto obs = internal::RemoveRedundantOrderBy(cs, qc.order_by());

  // Figure out whether the data is already ordered and which order we should
  // traverse the data.
  bool is_ordered, desc = false;
  std::tie(is_ordered, desc) = internal::IsOrdered(schema, obs);

  // Create the range iterator and if we are sorted, just return it.
  auto it = internal::CreateRangeIterator(schema, size, desc, cs, argv);
  if (is_ordered)
    return std::unique_ptr<RangeRowIterator>(
        new RangeRowIterator(std::move(it)));

  // Otherwise, create the sorted vector of indices and create the vector
  // iterator.
  return std::unique_ptr<VectorRowIterator>(new VectorRowIterator(
      internal::CreateSortedIndexVector(schema, std::move(it), obs)));
}

}  // namespace table_utils
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLE_UTILS_H_
