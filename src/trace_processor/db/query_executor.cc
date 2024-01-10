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

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numeric>
#include <optional>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column/arrangement_overlay.h"
#include "src/trace_processor/db/column/column.h"
#include "src/trace_processor/db/column/dense_null_overlay.h"
#include "src/trace_processor/db/column/dummy_storage.h"
#include "src/trace_processor/db/column/id_storage.h"
#include "src/trace_processor/db/column/null_overlay.h"
#include "src/trace_processor/db/column/numeric_storage.h"
#include "src/trace_processor/db/column/selector_overlay.h"
#include "src/trace_processor/db/column/set_id_storage.h"
#include "src/trace_processor/db/column/string_storage.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/query_executor.h"
#include "src/trace_processor/db/table.h"

namespace perfetto {
namespace trace_processor {

namespace {

using Range = RowMap::Range;
using Storage = column::Column;

}  // namespace

void QueryExecutor::FilterColumn(const Constraint& c,
                                 const column::Column& storage,
                                 RowMap* rm) {
  // Shortcut of empty row map.
  if (rm->empty())
    return;

  // Comparison of NULL with any operation apart from |IS_NULL| and
  // |IS_NOT_NULL| should return no rows.
  if (c.value.is_null() && c.op != FilterOp::kIsNull &&
      c.op != FilterOp::kIsNotNull) {
    rm->Clear();
    return;
  }

  switch (storage.ValidateSearchConstraints(c.value, c.op)) {
    case SearchValidationResult::kAllData:
      return;
    case SearchValidationResult::kNoData:
      rm->Clear();
      return;
    case SearchValidationResult::kOk:
      break;
  }

  uint32_t rm_size = rm->size();
  uint32_t rm_first = rm->Get(0);
  uint32_t rm_last = rm->Get(rm_size - 1);
  uint32_t range_size = rm_last - rm_first;

  // If the number of elements in the rowmap is small or the number of elements
  // is less than 1/10th of the range, use indexed filtering.
  // TODO(b/283763282): use Overlay estimations.
  bool disallows_index_search = rm->IsRange();
  bool prefers_index_search =
      rm->IsIndexVector() || rm_size < 1024 || rm_size * 10 < range_size;

  if (!disallows_index_search && prefers_index_search) {
    IndexSearch(c, storage, rm);
    return;
  }
  LinearSearch(c, storage, rm);
}

void QueryExecutor::LinearSearch(const Constraint& c,
                                 const column::Column& storage,
                                 RowMap* rm) {
  // TODO(b/283763282): Align these to word boundaries.
  Range bounds(rm->Get(0), rm->Get(rm->size() - 1) + 1);

  // Search the storage.
  RangeOrBitVector res = storage.Search(c.op, c.value, bounds);
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
                                const column::Column& storage,
                                RowMap* rm) {
  // Create outmost TableIndexVector.
  std::vector<uint32_t> table_indices = std::move(*rm).TakeAsIndexVector();

  RangeOrBitVector matched = storage.IndexSearch(
      c.op, c.value, table_indices.data(),
      static_cast<uint32_t>(table_indices.size()), false /* sorted */);

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
  // TODO(b/283763282): Remove after implementing extrinsic binary search.
  PERFETTO_FATAL("Extrinsic binary search is not implemented.");
}

RowMap QueryExecutor::FilterLegacy(const Table* table,
                                   const std::vector<Constraint>& c_vec) {
  RowMap rm(0, table->row_count());
  for (const auto& c : c_vec) {
    if (rm.empty()) {
      return rm;
    }
    const ColumnLegacy& col = table->columns()[c.col_idx];
    uint32_t column_size =
        col.IsId() ? col.overlay().row_map().Max() : col.storage_base().size();

    // RowMap size is 1.
    bool use_legacy = rm.size() == 1;

    // Storage has different size than Range overlay.
    use_legacy = use_legacy || (col.overlay().size() != column_size &&
                                col.overlay().row_map().IsRange());

    // Extrinsically sorted columns.
    use_legacy = use_legacy ||
                 (col.IsSorted() && col.overlay().row_map().IsIndexVector());

    if (use_legacy) {
      col.FilterInto(c.op, c.value, &rm);
      continue;
    }

    // Create storage
    std::unique_ptr<column::Column> storage;
    if (col.IsSetId()) {
      if (col.IsNullable()) {
        storage = std::make_unique<column::SetIdStorage>(
            &col.storage<std::optional<uint32_t>>().non_null_vector());
      } else {
        storage = std::make_unique<column::SetIdStorage>(
            &col.storage<uint32_t>().vector());
      }
    } else {
      switch (col.col_type()) {
        case ColumnType::kDummy:
          storage = std::make_unique<column::DummyStorage>();
          break;
        case ColumnType::kId:
          storage = std::make_unique<column::IdStorage>(column_size);
          break;
        case ColumnType::kString:
          storage = std::make_unique<column::StringStorage>(
              table->string_pool(), &col.storage<StringPool::Id>().vector(),
              col.IsSorted());
          break;
        case ColumnType::kInt64:
          if (col.IsNullable()) {
            storage = std::make_unique<column::NumericStorage<int64_t>>(
                &col.storage<std::optional<int64_t>>().non_null_vector(),
                col.col_type(), col.IsSorted());

          } else {
            storage = std::make_unique<column::NumericStorage<int64_t>>(
                &col.storage<int64_t>().vector(), col.col_type(),
                col.IsSorted());
          }
          break;
        case ColumnType::kUint32:
          if (col.IsNullable()) {
            storage = std::make_unique<column::NumericStorage<uint32_t>>(
                &col.storage<std::optional<uint32_t>>().non_null_vector(),
                col.col_type(), col.IsSorted());
          } else {
            storage = std::make_unique<column::NumericStorage<uint32_t>>(
                &col.storage<uint32_t>().vector(), col.col_type(),
                col.IsSorted());
          }
          break;
        case ColumnType::kInt32:
          if (col.IsNullable()) {
            storage = std::make_unique<column::NumericStorage<int32_t>>(
                &col.storage<std::optional<int32_t>>().non_null_vector(),
                col.col_type(), col.IsSorted());
          } else {
            storage = std::make_unique<column::NumericStorage<int32_t>>(
                &col.storage<int32_t>().vector(), col.col_type(),
                col.IsSorted());
          }
          break;
        case ColumnType::kDouble:
          if (col.IsNullable()) {
            storage = std::make_unique<column::NumericStorage<double>>(
                &col.storage<std::optional<double>>().non_null_vector(),
                col.col_type(), col.IsSorted());
          } else {
            storage = std::make_unique<column::NumericStorage<double>>(
                &col.storage<double>().vector(), col.col_type(),
                col.IsSorted());
          }
      }
    }
    if (col.IsNullable()) {
      // String columns are inherently nullable: null values are signified
      // with Id::Null().
      PERFETTO_CHECK(col.col_type() != ColumnType::kString);
      if (col.IsDense()) {
        storage = std::make_unique<column::DenseNullOverlay>(
            std::move(storage), col.storage_base().bv());
      } else {
        storage = std::make_unique<column::NullOverlay>(
            std::move(storage), col.storage_base().bv());
      }
    }
    if (col.overlay().row_map().IsIndexVector()) {
      storage = std::make_unique<column::ArrangementOverlay>(
          std::move(storage), col.overlay().row_map().GetIfIndexVector());
    }
    if (col.overlay().row_map().IsBitVector()) {
      storage = std::make_unique<column::SelectorOverlay>(
          std::move(storage), col.overlay().row_map().GetIfBitVector());
    }
    uint32_t pre_count = rm.size();
    FilterColumn(c, *storage.get(), &rm);
    PERFETTO_DCHECK(rm.size() <= pre_count);
  }
  return rm;
}

void QueryExecutor::BoundedColumnFilterForTesting(const Constraint& c,
                                                  const column::Column& col,
                                                  RowMap* rm) {
  switch (col.ValidateSearchConstraints(c.value, c.op)) {
    case SearchValidationResult::kAllData:
      return;
    case SearchValidationResult::kNoData:
      rm->Clear();
      return;
    case SearchValidationResult::kOk:
      break;
  }

  LinearSearch(c, col, rm);
}

void QueryExecutor::IndexedColumnFilterForTesting(const Constraint& c,
                                                  const column::Column& col,
                                                  RowMap* rm) {
  switch (col.ValidateSearchConstraints(c.value, c.op)) {
    case SearchValidationResult::kAllData:
      return;
    case SearchValidationResult::kNoData:
      rm->Clear();
      return;
    case SearchValidationResult::kOk:
      break;
  }

  IndexSearch(c, col, rm);
}

}  // namespace trace_processor
}  // namespace perfetto
