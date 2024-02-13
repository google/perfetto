/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/query_executor.h"
#include "src/trace_processor/db/table.h"

namespace perfetto::trace_processor {

namespace {

using Range = RowMap::Range;

}  // namespace

void QueryExecutor::FilterColumn(const Constraint& c,
                                 const column::DataLayerChain& chain,
                                 RowMap* rm) {
  // Shortcut of empty row map.
  uint32_t rm_size = rm->size();
  if (rm_size == 0)
    return;

  uint32_t rm_first = rm->Get(0);
  if (rm_size == 1) {
    switch (chain.SingleSearch(c.op, c.value, rm_first)) {
      case SingleSearchResult::kMatch:
        return;
      case SingleSearchResult::kNoMatch:
        rm->Clear();
        return;
      case SingleSearchResult::kNeedsFullSearch:
        break;
    }
  }

  // Comparison of NULL with any operation apart from |IS_NULL| and
  // |IS_NOT_NULL| should return no rows.
  if (c.value.is_null() && c.op != FilterOp::kIsNull &&
      c.op != FilterOp::kIsNotNull) {
    rm->Clear();
    return;
  }

  uint32_t rm_last = rm->Get(rm_size - 1);
  uint32_t range_size = rm_last - rm_first;

  // If the number of elements in the rowmap is small or the number of elements
  // is less than 1/10th of the range, use indexed filtering.
  // TODO(b/283763282): use Overlay estimations.
  bool disallows_index_search = rm->IsRange();
  bool prefers_index_search =
      rm->IsIndexVector() || rm_size < 1024 || rm_size * 10 < range_size;

  if (!disallows_index_search && prefers_index_search) {
    IndexSearch(c, chain, rm);
    return;
  }
  LinearSearch(c, chain, rm);
}

void QueryExecutor::LinearSearch(const Constraint& c,
                                 const column::DataLayerChain& chain,
                                 RowMap* rm) {
  // TODO(b/283763282): Align these to word boundaries.
  Range bounds(rm->Get(0), rm->Get(rm->size() - 1) + 1);

  // Search the storage.
  RangeOrBitVector res = chain.Search(c.op, c.value, bounds);
  if (rm->IsRange()) {
    if (res.IsRange()) {
      Range range = std::move(res).TakeIfRange();
      *rm = RowMap(range.start, range.end);
    } else {
      // The BitVector was already limited on the RowMap when created, so we can
      // take it as it is.
      *rm = RowMap(std::move(res).TakeIfBitVector());
    }
    return;
  }

  if (res.IsRange()) {
    Range range = std::move(res).TakeIfRange();
    rm->Intersect(RowMap(range.start, range.end));
    return;
  }
  rm->Intersect(RowMap(std::move(res).TakeIfBitVector()));
}

void QueryExecutor::IndexSearch(const Constraint& c,
                                const column::DataLayerChain& chain,
                                RowMap* rm) {
  // Create outmost TableIndexVector.
  std::vector<uint32_t> table_indices = std::move(*rm).TakeAsIndexVector();

  RangeOrBitVector matched = chain.IndexSearch(
      c.op, c.value,
      Indices{table_indices.data(), static_cast<uint32_t>(table_indices.size()),
              Indices::State::kMonotonic});

  if (matched.IsBitVector()) {
    BitVector res = std::move(matched).TakeIfBitVector();
    uint32_t i = 0;
    table_indices.erase(
        std::remove_if(table_indices.begin(), table_indices.end(),
                       [&i, &res](uint32_t) { return !res.IsSet(i++); }),
        table_indices.end());
    *rm = RowMap(std::move(table_indices));
    return;
  }

  Range res = std::move(matched).TakeIfRange();
  if (res.size() == 0) {
    rm->Clear();
    return;
  }
  if (res.size() == table_indices.size()) {
    return;
  }

  PERFETTO_DCHECK(res.end <= table_indices.size());
  std::vector<uint32_t> res_as_iv(
      table_indices.begin() + static_cast<int>(res.start),
      table_indices.begin() + static_cast<int>(res.end));
  *rm = RowMap(std::move(res_as_iv));
}

RowMap QueryExecutor::FilterLegacy(const Table* table,
                                   const std::vector<Constraint>& c_vec) {
  RowMap rm(0, table->row_count());
  for (const auto& c : c_vec) {
    FilterColumn(c, table->ChainForColumn(c.col_idx), &rm);
  }
  return rm;
}

void QueryExecutor::BoundedColumnFilterForTesting(
    const Constraint& c,
    const column::DataLayerChain& col,
    RowMap* rm) {
  LinearSearch(c, col, rm);
}

void QueryExecutor::IndexedColumnFilterForTesting(
    const Constraint& c,
    const column::DataLayerChain& col,
    RowMap* rm) {
  IndexSearch(c, col, rm);
}

}  // namespace perfetto::trace_processor
