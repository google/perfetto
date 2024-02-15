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

#include "src/trace_processor/db/column/set_id_storage.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "src/trace_processor/tp_metatrace.h"

#include "protos/perfetto/trace_processor/metatrace_categories.pbzero.h"
#include "protos/perfetto/trace_processor/serialization.pbzero.h"

namespace perfetto::trace_processor::column {
namespace {

using SetId = SetIdStorage::SetId;

uint32_t UpperBoundIntrinsic(const SetId* data, SetId id, Range range) {
  if (id >= range.end) {
    return range.end;
  }
  const auto* upper =
      std::upper_bound(data + std::max(range.start, id), data + range.end, id);
  return static_cast<uint32_t>(std::distance(data, upper));
}

uint32_t LowerBoundIntrinsic(const SetId* data, SetId id, Range range) {
  if (data[range.start] == id) {
    return range.start;
  }
  if (range.Contains(id) && data[id] == id) {
    return id;
  }
  // If none of the above are true, than |id| is not present in data, so we need
  // to look for the first value higher than |id|.
  return UpperBoundIntrinsic(data, id, range);
}

}  // namespace

SetIdStorage::SetIdStorage(const std::vector<uint32_t>* values)
    : values_(values) {}

SetIdStorage::ChainImpl::ChainImpl(const std::vector<uint32_t>* values)
    : values_(values) {}

SingleSearchResult SetIdStorage::ChainImpl::SingleSearch(FilterOp op,
                                                         SqlValue sql_val,
                                                         uint32_t i) const {
  if (sql_val.type != SqlValue::kLong ||
      sql_val.long_value > std::numeric_limits<uint32_t>::max() ||
      sql_val.long_value < std::numeric_limits<uint32_t>::min()) {
    // Because of the large amount of code needing for handling comparisions
    // with doubles or out of range values, just defer to the full search.
    return SingleSearchResult::kNeedsFullSearch;
  }
  return utils::SingleSearchNumeric(op, (*values_)[i],
                                    static_cast<uint32_t>(sql_val.long_value));
}

SearchValidationResult SetIdStorage::ChainImpl::ValidateSearchConstraints(
    FilterOp op,
    SqlValue val) const {
  // NULL checks.
  if (PERFETTO_UNLIKELY(val.is_null())) {
    if (op == FilterOp::kIsNotNull) {
      return SearchValidationResult::kAllData;
    }
    if (op == FilterOp::kIsNull) {
      return SearchValidationResult::kNoData;
    }
    PERFETTO_FATAL(
        "Invalid filter operation. NULL should only be compared with 'IS NULL' "
        "and 'IS NOT NULL'");
  }

  // FilterOp checks. Switch so that we get a warning if new FilterOp is not
  // handled.
  switch (op) {
    case FilterOp::kEq:
    case FilterOp::kNe:
    case FilterOp::kLt:
    case FilterOp::kLe:
    case FilterOp::kGt:
    case FilterOp::kGe:
      break;
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
      PERFETTO_FATAL("Invalid constraints.");
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      return SearchValidationResult::kNoData;
  }

  // Type checks.
  switch (val.type) {
    case SqlValue::kNull:
    case SqlValue::kLong:
    case SqlValue::kDouble:
      break;
    case SqlValue::kString:
      // Any string is always more than any numeric.
      if (op == FilterOp::kLt || op == FilterOp::kLe) {
        return SearchValidationResult::kAllData;
      }
      return SearchValidationResult::kNoData;
    case SqlValue::kBytes:
      return SearchValidationResult::kNoData;
  }

  // Bounds of the value.
  double num_val = val.type == SqlValue::kLong
                       ? static_cast<double>(val.AsLong())
                       : val.AsDouble();
  if (PERFETTO_UNLIKELY(num_val > std::numeric_limits<uint32_t>::max())) {
    if (op == FilterOp::kLe || op == FilterOp::kLt || op == FilterOp::kNe) {
      return SearchValidationResult::kAllData;
    }
    return SearchValidationResult::kNoData;
  }
  if (PERFETTO_UNLIKELY(num_val < std::numeric_limits<uint32_t>::min())) {
    if (op == FilterOp::kGe || op == FilterOp::kGt || op == FilterOp::kNe) {
      return SearchValidationResult::kAllData;
    }
    return SearchValidationResult::kNoData;
  }

  return SearchValidationResult::kOk;
}

RangeOrBitVector SetIdStorage::ChainImpl::SearchValidated(
    FilterOp op,
    SqlValue sql_val,
    Range search_range) const {
  PERFETTO_DCHECK(search_range.end <= size());

  PERFETTO_TP_TRACE(metatrace::Category::DB, "SetIdStorage::ChainImpl::Search",
                    [&search_range, op](metatrace::Record* r) {
                      r->AddArg("Start", std::to_string(search_range.start));
                      r->AddArg("End", std::to_string(search_range.end));
                      r->AddArg("Op",
                                std::to_string(static_cast<uint32_t>(op)));
                    });

  // It's a valid filter operation if |sql_val| is a double, although it
  // requires special logic.
  if (sql_val.type == SqlValue::kDouble) {
    switch (utils::CompareIntColumnWithDouble(op, &sql_val)) {
      case SearchValidationResult::kOk:
        break;
      case SearchValidationResult::kAllData:
        return RangeOrBitVector(Range(0, search_range.end));
      case SearchValidationResult::kNoData:
        return RangeOrBitVector(Range());
    }
  }

  auto val = static_cast<uint32_t>(sql_val.AsLong());
  if (op == FilterOp::kNe) {
    // Not equal is a special operation on binary search, as it doesn't define a
    // range, and rather just `not` range returned with `equal` operation.
    Range eq_range = BinarySearchIntrinsic(FilterOp::kEq, val, search_range);
    BitVector bv(search_range.start, false);
    bv.Resize(eq_range.start, true);
    bv.Resize(eq_range.end, false);
    bv.Resize(search_range.end, true);
    return RangeOrBitVector(std::move(bv));
  }
  return RangeOrBitVector(BinarySearchIntrinsic(op, val, search_range));
}

RangeOrBitVector SetIdStorage::ChainImpl::IndexSearchValidated(
    FilterOp op,
    SqlValue sql_val,
    Indices indices) const {
  PERFETTO_TP_TRACE(
      metatrace::Category::DB, "SetIdStorage::ChainImpl::IndexSearch",
      [indices, op](metatrace::Record* r) {
        r->AddArg("Count", std::to_string(indices.size));
        r->AddArg("Op", std::to_string(static_cast<uint32_t>(op)));
      });

  // It's a valid filter operation if |sql_val| is a double, although it
  // requires special logic.
  if (sql_val.type == SqlValue::kDouble) {
    switch (utils::CompareIntColumnWithDouble(op, &sql_val)) {
      case SearchValidationResult::kOk:
        break;
      case SearchValidationResult::kAllData:
        return RangeOrBitVector(Range(0, indices.size));
      case SearchValidationResult::kNoData:
        return RangeOrBitVector(Range());
    }
  }

  auto val = static_cast<uint32_t>(sql_val.AsLong());
  BitVector::Builder builder(indices.size);

  // TODO(mayzner): Instead of utils::IndexSearchWithComparator, use the
  // property of SetId data - that for each index i, data[i] <= i.
  switch (op) {
    case FilterOp::kEq:
      utils::IndexSearchWithComparator(val, values_->data(), indices.data,
                                       std::equal_to<>(), builder);
      break;
    case FilterOp::kNe:
      utils::IndexSearchWithComparator(val, values_->data(), indices.data,
                                       std::not_equal_to<>(), builder);
      break;
    case FilterOp::kLe:
      utils::IndexSearchWithComparator(val, values_->data(), indices.data,
                                       std::less_equal<>(), builder);
      break;
    case FilterOp::kLt:
      utils::IndexSearchWithComparator(val, values_->data(), indices.data,
                                       std::less<>(), builder);
      break;
    case FilterOp::kGt:
      utils::IndexSearchWithComparator(val, values_->data(), indices.data,
                                       std::greater<>(), builder);
      break;
    case FilterOp::kGe:
      utils::IndexSearchWithComparator(val, values_->data(), indices.data,
                                       std::greater_equal<>(), builder);
      break;
    case FilterOp::kIsNotNull:
      return RangeOrBitVector(Range(0, indices.size));
    case FilterOp::kIsNull:
      return RangeOrBitVector(Range());
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      PERFETTO_FATAL("Illegal argument");
  }
  return RangeOrBitVector(std::move(builder).Build());
}

Range SetIdStorage::ChainImpl::OrderedIndexSearchValidated(
    FilterOp op,
    SqlValue sql_val,
    Indices indices) const {
  // Indices are monotonic non-contiguous values.
  auto res = SearchValidated(
      op, sql_val, Range(indices.data[0], indices.data[indices.size - 1] + 1));
  PERFETTO_CHECK(res.IsRange());
  Range res_range = std::move(res).TakeIfRange();

  const auto* start_ptr = std::lower_bound(
      indices.data, indices.data + indices.size, res_range.start);
  const auto* end_ptr =
      std::lower_bound(start_ptr, indices.data + indices.size, res_range.end);

  return {static_cast<uint32_t>(std::distance(indices.data, start_ptr)),
          static_cast<uint32_t>(std::distance(indices.data, end_ptr))};
}

Range SetIdStorage::ChainImpl::BinarySearchIntrinsic(FilterOp op,
                                                     SetId val,
                                                     Range range) const {
  switch (op) {
    case FilterOp::kEq:
      return {LowerBoundIntrinsic(values_->data(), val, range),
              UpperBoundIntrinsic(values_->data(), val, range)};
    case FilterOp::kLe: {
      return {range.start, UpperBoundIntrinsic(values_->data(), val, range)};
    }
    case FilterOp::kLt:
      return {range.start, LowerBoundIntrinsic(values_->data(), val, range)};
    case FilterOp::kGe:
      return {LowerBoundIntrinsic(values_->data(), val, range), range.end};
    case FilterOp::kGt:
      return {UpperBoundIntrinsic(values_->data(), val, range), range.end};
    case FilterOp::kIsNotNull:
      return range;
    case FilterOp::kNe:
      PERFETTO_FATAL("Shouldn't be called");
    case FilterOp::kIsNull:
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      return {};
  }
  return {};
}

void SetIdStorage::ChainImpl::StableSort(SortToken* start,
                                         SortToken* end,
                                         SortDirection direction) const {
  switch (direction) {
    case SortDirection::kAscending:
      std::stable_sort(start, end,
                       [this](const SortToken& a, const SortToken& b) {
                         return (*values_)[a.index] < (*values_)[b.index];
                       });
      break;
    case SortDirection::kDescending:
      std::stable_sort(start, end,
                       [this](const SortToken& a, const SortToken& b) {
                         return (*values_)[a.index] > (*values_)[b.index];
                       });
      break;
  }
}

void SetIdStorage::ChainImpl::Serialize(StorageProto* msg) const {
  auto* vec_msg = msg->set_set_id_storage();
  vec_msg->set_values(reinterpret_cast<const uint8_t*>(values_->data()),
                      sizeof(SetId) * size());
}

}  // namespace perfetto::trace_processor::column
