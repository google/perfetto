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
#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_UTILS_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_UTILS_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto::trace_processor::column::utils {

template <typename Comparator, typename ValType, typename DataType>
void LinearSearchWithComparator(ValType val,
                                const DataType* data_ptr,
                                Comparator comparator,
                                BitVector::Builder& builder) {
  // Slow path: we compare <64 elements and append to get us to a word
  // boundary.
  const DataType* cur_val = data_ptr;
  uint32_t front_elements = builder.BitsUntilWordBoundaryOrFull();
  for (uint32_t i = 0; i < front_elements; ++i, ++cur_val) {
    builder.Append(comparator(*cur_val, val));
  }

  // Fast path: we compare as many groups of 64 elements as we can.
  // This should be very easy for the compiler to auto-vectorize.
  uint32_t fast_path_elements = builder.BitsInCompleteWordsUntilFull();
  for (uint32_t i = 0; i < fast_path_elements; i += BitVector::kBitsInWord) {
    uint64_t word = 0;
    // This part should be optimised by SIMD and is expected to be fast.
    for (uint32_t k = 0; k < BitVector::kBitsInWord; ++k, ++cur_val) {
      bool comp_result = comparator(*cur_val, val);
      word |= static_cast<uint64_t>(comp_result) << k;
    }
    builder.AppendWord(word);
  }

  // Slow path: we compare <64 elements and append to fill the Builder.
  uint32_t back_elements = builder.BitsUntilFull();
  for (uint32_t i = 0; i < back_elements; ++i, ++cur_val) {
    builder.Append(comparator(*cur_val, val));
  }
}

template <typename Comparator, typename ValType, typename DataType>
void IndexSearchWithComparator(ValType val,
                               const DataType* data_ptr,
                               const uint32_t* indices,
                               Comparator comparator,
                               BitVector::Builder& builder) {
  // Fast path: we compare as many groups of 64 elements as we can.
  // This should be very easy for the compiler to auto-vectorize.
  const uint32_t* cur_idx = indices;
  uint32_t fast_path_elements = builder.BitsInCompleteWordsUntilFull();
  for (uint32_t i = 0; i < fast_path_elements; i += BitVector::kBitsInWord) {
    uint64_t word = 0;
    // This part should be optimised by SIMD and is expected to be fast.
    for (uint32_t k = 0; k < BitVector::kBitsInWord; ++k, ++cur_idx) {
      bool comp_result = comparator(*(data_ptr + *cur_idx), val);
      word |= static_cast<uint64_t>(comp_result) << k;
    }
    builder.AppendWord(word);
  }

  // Slow path: we compare <64 elements and append to fill the Builder.
  uint32_t back_elements = builder.BitsUntilFull();
  for (uint32_t i = 0; i < back_elements; ++i, ++cur_idx) {
    builder.Append(comparator(*(data_ptr + *cur_idx), val));
  }
}

template <typename T>
SingleSearchResult SingleSearchNumeric(FilterOp op, T left, T right) {
  switch (op) {
    case FilterOp::kEq:
      return std::equal_to<T>()(left, right) ? SingleSearchResult::kMatch
                                             : SingleSearchResult::kNoMatch;
    case FilterOp::kNe:
      return std::not_equal_to<T>()(left, right) ? SingleSearchResult::kMatch
                                                 : SingleSearchResult::kNoMatch;
    case FilterOp::kGe:
      return std::greater_equal<T>()(left, right)
                 ? SingleSearchResult::kMatch
                 : SingleSearchResult::kNoMatch;
    case FilterOp::kGt:
      return std::greater<T>()(left, right) ? SingleSearchResult::kMatch
                                            : SingleSearchResult::kNoMatch;
    case FilterOp::kLe:
      return std::less_equal<T>()(left, right) ? SingleSearchResult::kMatch
                                               : SingleSearchResult::kNoMatch;
    case FilterOp::kLt:
      return std::less<T>()(left, right) ? SingleSearchResult::kMatch
                                         : SingleSearchResult::kNoMatch;
    case FilterOp::kIsNotNull:
      return SingleSearchResult::kMatch;
    case FilterOp::kGlob:
    case FilterOp::kRegex:
    case FilterOp::kIsNull:
      return SingleSearchResult::kNoMatch;
  }
  PERFETTO_FATAL("For GCC");
}

// Used for comparing the integer column ({u|}int{32|64}) with a double value.
// If further search is required it would return kOk and change the SqlValue to
// a `SqlLong` which would return real results.
SearchValidationResult CompareIntColumnWithDouble(FilterOp op,
                                                  SqlValue* sql_val);

// If the validation result doesn't require further search, it will return a
// Range that can be passed further. Else it returns nullopt.
std::optional<Range> CanReturnEarly(SearchValidationResult, Range);

// If the validation result doesn't require further search, it will return a
// Range that can be passed further. Else it returns nullopt.
std::optional<Range> CanReturnEarly(SearchValidationResult,
                                    uint32_t indices_size);

std::vector<uint32_t> ExtractPayloadForTesting(
    std::vector<column::DataLayerChain::SortToken>&);

std::vector<uint32_t> ToIndexVectorForTests(RangeOrBitVector&);

}  // namespace perfetto::trace_processor::column::utils

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_UTILS_H_
