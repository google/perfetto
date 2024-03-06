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

#include "src/trace_processor/db/column/id_storage.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>

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

template <typename Comparator>
RangeOrBitVector IndexSearchWithComparator(uint32_t val,
                                           const uint32_t* indices,
                                           uint32_t indices_size,
                                           Comparator comparator) {
  // Slow path: we compare <64 elements and append to get us to a word
  // boundary.
  const uint32_t* ptr = indices;
  BitVector::Builder builder(indices_size);
  uint32_t front_elements = builder.BitsUntilWordBoundaryOrFull();
  for (uint32_t i = 0; i < front_elements; ++i) {
    builder.Append(comparator(ptr[i], val));
  }
  ptr += front_elements;

  // Fast path: we compare as many groups of 64 elements as we can.
  // This should be very easy for the compiler to auto-vectorize.
  uint32_t fast_path_elements = builder.BitsInCompleteWordsUntilFull();
  for (uint32_t i = 0; i < fast_path_elements; i += BitVector::kBitsInWord) {
    uint64_t word = 0;
    // This part should be optimised by SIMD and is expected to be fast.
    for (uint32_t k = 0; k < BitVector::kBitsInWord; ++k) {
      bool comp_result = comparator(ptr[i + k], val);
      word |= static_cast<uint64_t>(comp_result) << k;
    }
    builder.AppendWord(word);
  }
  ptr += fast_path_elements;

  // Slow path: we compare <64 elements and append to fill the Builder.
  uint32_t back_elements = builder.BitsUntilFull();
  for (uint32_t i = 0; i < back_elements; ++i) {
    builder.Append(comparator(ptr[i], val));
  }
  return RangeOrBitVector(std::move(builder).Build());
}

}  // namespace

SearchValidationResult IdStorage::ChainImpl::ValidateSearchConstraints(
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
    PERFETTO_DFATAL(
        "Invalid filter operation. NULL should only be compared with 'IS NULL' "
        "and 'IS NOT NULL'");
    return SearchValidationResult::kNoData;
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
      PERFETTO_FATAL("Invalid constraint");
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

SingleSearchResult IdStorage::ChainImpl::SingleSearch(FilterOp op,
                                                      SqlValue sql_val,
                                                      uint32_t index) const {
  if (sql_val.type != SqlValue::kLong ||
      sql_val.long_value > std::numeric_limits<uint32_t>::max() ||
      sql_val.long_value < std::numeric_limits<uint32_t>::min()) {
    // Because of the large amount of code needing for handling comparisions
    // with doubles or out of range values, just defer to the full search.
    return SingleSearchResult::kNeedsFullSearch;
  }
  auto val = static_cast<uint32_t>(sql_val.long_value);
  switch (op) {
    case FilterOp::kEq:
      return index == val ? SingleSearchResult::kMatch
                          : SingleSearchResult::kNoMatch;
    case FilterOp::kNe:
      return index != val ? SingleSearchResult::kMatch
                          : SingleSearchResult::kNoMatch;
    case FilterOp::kGe:
      return index >= val ? SingleSearchResult::kMatch
                          : SingleSearchResult::kNoMatch;
    case FilterOp::kGt:
      return index > val ? SingleSearchResult::kMatch
                         : SingleSearchResult::kNoMatch;
    case FilterOp::kLe:
      return index <= val ? SingleSearchResult::kMatch
                          : SingleSearchResult::kNoMatch;
    case FilterOp::kLt:
      return index < val ? SingleSearchResult::kMatch
                         : SingleSearchResult::kNoMatch;
    case FilterOp::kIsNotNull:
      return SingleSearchResult::kMatch;
    case FilterOp::kIsNull:
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      return SingleSearchResult::kNoMatch;
  }
  PERFETTO_FATAL("For GCC");
}

RangeOrBitVector IdStorage::ChainImpl::SearchValidated(
    FilterOp op,
    SqlValue sql_val,
    Range search_range) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "IdStorage::ChainImpl::Search",
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
    BitVector ret(search_range.start, false);
    ret.Resize(search_range.end, true);
    ret.Clear(val);
    return RangeOrBitVector(std::move(ret));
  }
  return RangeOrBitVector(BinarySearchIntrinsic(op, val, search_range));
}

RangeOrBitVector IdStorage::ChainImpl::IndexSearchValidated(
    FilterOp op,
    SqlValue sql_val,
    Indices indices) const {
  PERFETTO_TP_TRACE(
      metatrace::Category::DB, "IdStorage::ChainImpl::IndexSearch",
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
  switch (op) {
    case FilterOp::kEq:
      return IndexSearchWithComparator(val, indices.data, indices.size,
                                       std::equal_to<>());
    case FilterOp::kNe:
      return IndexSearchWithComparator(val, indices.data, indices.size,
                                       std::not_equal_to<>());
    case FilterOp::kLe:
      return IndexSearchWithComparator(val, indices.data, indices.size,
                                       std::less_equal<>());
    case FilterOp::kLt:
      return IndexSearchWithComparator(val, indices.data, indices.size,
                                       std::less<>());
    case FilterOp::kGt:
      return IndexSearchWithComparator(val, indices.data, indices.size,
                                       std::greater<>());
    case FilterOp::kGe:
      return IndexSearchWithComparator(val, indices.data, indices.size,
                                       std::greater_equal<>());
    case FilterOp::kIsNotNull:
    case FilterOp::kIsNull:
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      PERFETTO_FATAL("Invalid filter operation");
  }
  PERFETTO_FATAL("FilterOp not matched");
}

Range IdStorage::ChainImpl::OrderedIndexSearchValidated(FilterOp op,
                                                        SqlValue sql_val,
                                                        Indices indices) const {
  PERFETTO_DCHECK(op != FilterOp::kNe);

  PERFETTO_TP_TRACE(
      metatrace::Category::DB, "IdStorage::ChainImpl::OrderedIndexSearch",
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
        return {0, indices.size};
      case SearchValidationResult::kNoData:
        return {};
    }
  }
  auto val = static_cast<uint32_t>(sql_val.AsLong());

  // Indices are monotonic non contiguous values if OrderedIndexSearch was
  // called.
  // Look for the first and last index and find the result of looking for this
  // range in IdStorage.
  Range indices_range(indices.data[0], indices.data[indices.size - 1] + 1);
  Range bin_search_ret = BinarySearchIntrinsic(op, val, indices_range);

  const auto* start_ptr = std::lower_bound(
      indices.data, indices.data + indices.size, bin_search_ret.start);
  const auto* end_ptr = std::lower_bound(start_ptr, indices.data + indices.size,
                                         bin_search_ret.end);
  return {static_cast<uint32_t>(std::distance(indices.data, start_ptr)),
          static_cast<uint32_t>(std::distance(indices.data, end_ptr))};
}

Range IdStorage::ChainImpl::BinarySearchIntrinsic(FilterOp op,
                                                  Id val,
                                                  Range range) {
  switch (op) {
    case FilterOp::kEq:
      return {val, val + (range.start <= val && val < range.end)};
    case FilterOp::kLe:
      return {range.start, std::min(val + 1, range.end)};
    case FilterOp::kLt:
      return {range.start, std::min(val, range.end)};
    case FilterOp::kGe:
      return {std::max(val, range.start), range.end};
    case FilterOp::kGt:
      return {std::max(val + 1, range.start), range.end};
    case FilterOp::kIsNotNull:
    case FilterOp::kNe:
    case FilterOp::kIsNull:
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      PERFETTO_FATAL("Invalid filter operation");
  }
  PERFETTO_FATAL("FilterOp not matched");
}

void IdStorage::ChainImpl::StableSort(SortToken* start,
                                      SortToken* end,
                                      SortDirection direction) const {
  switch (direction) {
    case SortDirection::kAscending:
      std::stable_sort(start, end, [](const SortToken& a, const SortToken& b) {
        return a.index < b.index;
      });
      return;
    case SortDirection::kDescending:
      std::stable_sort(start, end, [](const SortToken& a, const SortToken& b) {
        return a.index > b.index;
      });
      return;
  }
  PERFETTO_FATAL("For GCC");
}

void IdStorage::ChainImpl::Serialize(StorageProto* storage) const {
  storage->set_id_storage();
}

}  // namespace perfetto::trace_processor::column
