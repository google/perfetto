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

#include "src/trace_processor/db/column/utils.h"

namespace perfetto {
namespace trace_processor {
namespace column {
namespace utils {

SearchValidationResult CompareIntColumnWithDouble(SqlValue* sql_val,
                                                  FilterOp op) {
  double double_val = sql_val->AsDouble();
  if (std::equal_to<double>()(
          double_val, static_cast<double>(static_cast<uint32_t>(double_val)))) {
    // If double is the same as uint32_t, we should just "cast" the |sql_val|
    // to be treated as long.
    *sql_val = SqlValue::Long(static_cast<int64_t>(double_val));
    return SearchValidationResult::kOk;
  }
  // Logic for when the value is a real double.
  switch (op) {
    case FilterOp::kEq:
      return SearchValidationResult::kNoData;
    case FilterOp::kNe:
      return SearchValidationResult::kAllData;

    case FilterOp::kLe:
    case FilterOp::kGt:
      *sql_val = SqlValue::Long(static_cast<int64_t>(std::floor(double_val)));
      return SearchValidationResult::kOk;

    case FilterOp::kLt:
    case FilterOp::kGe:
      *sql_val = SqlValue::Long(static_cast<int64_t>(std::ceil(double_val)));
      return SearchValidationResult::kOk;

    case FilterOp::kIsNotNull:
    case FilterOp::kIsNull:
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      PERFETTO_FATAL("Invalid filter operation");
  }
  PERFETTO_FATAL("For GCC");
}

std::vector<uint32_t> ToIndexVectorForTests(RangeOrBitVector& r_or_bv) {
  RowMap rm;
  if (r_or_bv.IsBitVector()) {
    rm = RowMap(std::move(r_or_bv).TakeIfBitVector());
  } else {
    Range range = std::move(r_or_bv).TakeIfRange();
    rm = RowMap(range.start, range.end);
  }
  return rm.GetAllIndices();
}

}  // namespace utils

}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
