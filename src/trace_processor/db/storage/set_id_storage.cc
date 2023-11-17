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

#include "src/trace_processor/db/storage/set_id_storage.h"
#include <functional>
#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/storage/types.h"
#include "src/trace_processor/db/storage/utils.h"
#include "src/trace_processor/tp_metatrace.h"

namespace perfetto {
namespace trace_processor {
namespace storage {

namespace {

using Range = RowMap::Range;
using SetId = SetIdStorage::SetId;

uint32_t UpperBoundIntrinsic(const SetId* data, SetId id, RowMap::Range range) {
  if (id >= range.end) {
    return range.end;
  }
  auto upper =
      std::upper_bound(data + std::max(range.start, id), data + range.end, id);
  return static_cast<uint32_t>(std::distance(data, upper));
}

uint32_t LowerBoundIntrinsic(const SetId* data, SetId id, RowMap::Range range) {
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

RangeOrBitVector SetIdStorage::Search(FilterOp op,
                                      SqlValue sql_val,
                                      RowMap::Range range) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "SetIdStorage::Search",
                    [&range, op](metatrace::Record* r) {
                      r->AddArg("Start", std::to_string(range.start));
                      r->AddArg("End", std::to_string(range.end));
                      r->AddArg("Op",
                                std::to_string(static_cast<uint32_t>(op)));
                    });

  PERFETTO_DCHECK(range.end <= size());

  if (op == FilterOp::kNe) {
    if (sql_val.is_null()) {
      return RangeOrBitVector(Range());
    }
    // Not equal is a special operation on binary search, as it doesn't define a
    // range, and rather just `not` range returned with `equal` operation.
    RowMap::Range eq_range =
        BinarySearchIntrinsic(FilterOp::kEq, sql_val, range);
    BitVector bv(eq_range.start, true);
    bv.Resize(eq_range.end);
    bv.Resize(std::min(range.end - 1, eq_range.end), true);
    return RangeOrBitVector(std::move(bv));
  }
  return RangeOrBitVector(BinarySearchIntrinsic(op, sql_val, range));
}

RangeOrBitVector SetIdStorage::IndexSearch(FilterOp op,
                                           SqlValue sql_val,
                                           uint32_t* indices,
                                           uint32_t indices_count,
                                           bool) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "SetIdStorage::IndexSearch",
                    [indices_count, op](metatrace::Record* r) {
                      r->AddArg("Count", std::to_string(indices_count));
                      r->AddArg("Op",
                                std::to_string(static_cast<uint32_t>(op)));
                    });

  // Validate sql_val
  if (PERFETTO_UNLIKELY(sql_val.is_null())) {
    if (op == FilterOp::kIsNotNull) {
      return RangeOrBitVector(Range(indices_count, true));
    }
    return RangeOrBitVector(Range());
  }

  if (PERFETTO_UNLIKELY(sql_val.AsLong() >
                        std::numeric_limits<uint32_t>::max())) {
    if (op == FilterOp::kLe || op == FilterOp::kLt) {
      return RangeOrBitVector(Range(indices_count, true));
    }
    return RangeOrBitVector(Range());
  }

  if (PERFETTO_UNLIKELY(sql_val.AsLong() <
                        std::numeric_limits<uint32_t>::min())) {
    if (op == FilterOp::kGe || op == FilterOp::kGt) {
      return RangeOrBitVector(Range(indices_count, true));
    }
    return RangeOrBitVector(Range());
  }
  uint32_t val = static_cast<uint32_t>(sql_val.AsLong());

  BitVector::Builder builder(indices_count);

  // TODO(mayzner): Instead of utils::IndexSearchWithComparator, use the
  // property of SetId data - that for each index i, data[i] <= i.
  switch (op) {
    case FilterOp::kEq:
      utils::IndexSearchWithComparator(val, values_->data(), indices,
                                       std::equal_to<uint32_t>(), builder);
      break;
    case FilterOp::kNe:
      utils::IndexSearchWithComparator(val, values_->data(), indices,
                                       std::not_equal_to<uint32_t>(), builder);
      break;
    case FilterOp::kLe:
      utils::IndexSearchWithComparator(val, values_->data(), indices,
                                       std::less_equal<uint32_t>(), builder);
      break;
    case FilterOp::kLt:
      utils::IndexSearchWithComparator(val, values_->data(), indices,
                                       std::less<uint32_t>(), builder);
      break;
    case FilterOp::kGt:
      utils::IndexSearchWithComparator(val, values_->data(), indices,
                                       std::greater<uint32_t>(), builder);
      break;
    case FilterOp::kGe:
      utils::IndexSearchWithComparator(val, values_->data(), indices,
                                       std::greater_equal<uint32_t>(), builder);
      break;
    case FilterOp::kIsNotNull:
      return RangeOrBitVector(Range(0, indices_count));
    case FilterOp::kIsNull:
      return RangeOrBitVector(Range());
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      PERFETTO_FATAL("Illegal argument");
  }
  return RangeOrBitVector(std::move(builder).Build());
}

Range SetIdStorage::BinarySearchIntrinsic(FilterOp op,
                                          SqlValue sql_val,
                                          Range range) const {
  // Validate sql_value
  if (PERFETTO_UNLIKELY(sql_val.is_null())) {
    if (op == FilterOp::kIsNotNull) {
      return range;
    }
    return Range();
  }

  if (PERFETTO_UNLIKELY(sql_val.AsLong() >
                        std::numeric_limits<uint32_t>::max())) {
    if (op == FilterOp::kLe || op == FilterOp::kLt) {
      return range;
    }
    return Range();
  }

  if (PERFETTO_UNLIKELY(sql_val.AsLong() <
                        std::numeric_limits<uint32_t>::min())) {
    if (op == FilterOp::kGe || op == FilterOp::kGt) {
      return range;
    }
    return Range();
  }

  uint32_t val = static_cast<uint32_t>(sql_val.AsLong());

  switch (op) {
    case FilterOp::kEq:
      return Range(LowerBoundIntrinsic(values_->data(), val, range),
                   UpperBoundIntrinsic(values_->data(), val, range));
    case FilterOp::kLe: {
      return Range(range.start,
                   UpperBoundIntrinsic(values_->data(), val, range));
    }
    case FilterOp::kLt:
      return Range(range.start,
                   LowerBoundIntrinsic(values_->data(), val, range));
    case FilterOp::kGe:
      return Range(LowerBoundIntrinsic(values_->data(), val, range), range.end);
    case FilterOp::kGt:
      return Range(UpperBoundIntrinsic(values_->data(), val, range), range.end);
    case FilterOp::kIsNotNull:
      return range;
    case FilterOp::kNe:
      PERFETTO_FATAL("Shouldn't be called");
    case FilterOp::kIsNull:
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      return Range();
  }
  return Range();
}

void SetIdStorage::StableSort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_ELOG("Not implemented");
}

void SetIdStorage::Sort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_ELOG("Not implemented");
}

}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
