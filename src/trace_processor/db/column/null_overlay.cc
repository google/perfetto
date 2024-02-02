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

#include "src/trace_processor/db/column/null_overlay.h"

#include <cstdint>

#include "protos/perfetto/trace_processor/serialization.pbzero.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/column/column.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/tp_metatrace.h"

namespace perfetto {
namespace trace_processor {
namespace column {
namespace {

BitVector ReconcileStorageResult(FilterOp op,
                                 const BitVector& non_null,
                                 RangeOrBitVector storage_result,
                                 Range in_range) {
  PERFETTO_CHECK(in_range.end <= non_null.size());

  // Reconcile the results of the Search operation with the non-null indices
  // to ensure only those positions are set.
  BitVector res;
  if (storage_result.IsRange()) {
    Range range = std::move(storage_result).TakeIfRange();
    if (range.size() > 0) {
      res = non_null.IntersectRange(non_null.IndexOfNthSet(range.start),
                                    non_null.IndexOfNthSet(range.end - 1) + 1);

      // We should always have at least as many elements as the input range
      // itself.
      PERFETTO_CHECK(res.size() <= in_range.end);
    }
  } else {
    res = non_null.Copy();
    res.UpdateSetBits(std::move(storage_result).TakeIfBitVector());
  }

  // Ensure that |res| exactly matches the size which we need to return,
  // padding with zeros or truncating if necessary.
  res.Resize(in_range.end, false);

  // For the IS NULL constraint, we also need to include all the null indices
  // themselves.
  if (PERFETTO_UNLIKELY(op == FilterOp::kIsNull)) {
    BitVector null = non_null.IntersectRange(in_range.start, in_range.end);
    null.Resize(in_range.end, false);
    null.Not();
    res.Or(null);
  }
  return res;
}

}  // namespace

SearchValidationResult NullOverlay::ValidateSearchConstraints(
    SqlValue sql_val,
    FilterOp op) const {
  if (op == FilterOp::kIsNull) {
    return SearchValidationResult::kOk;
  }

  return storage_->ValidateSearchConstraints(sql_val, op);
}

NullOverlay::NullOverlay(std::unique_ptr<Column> storage,
                         const BitVector* non_null)
    : storage_(std::move(storage)), non_null_(non_null) {
  PERFETTO_DCHECK(non_null_->CountSetBits() <= storage_->size());
}

RangeOrBitVector NullOverlay::Search(FilterOp op,
                                     SqlValue sql_val,
                                     Range in) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "NullOverlay::Search");

  if (op == FilterOp::kIsNull) {
    switch (storage_->ValidateSearchConstraints(sql_val, op)) {
      case SearchValidationResult::kNoData: {
        // There is no need to search in underlying storage. It's enough to
        // intersect the |non_null_|.
        BitVector res = non_null_->IntersectRange(in.start, in.end);
        res.Not();
        res.Resize(in.end, false);
        return RangeOrBitVector(std::move(res));
      }
      case SearchValidationResult::kAllData:
        return RangeOrBitVector(in);
      case SearchValidationResult::kOk:
        break;
    }
  }

  // Figure out the bounds of the indices in the underlying storage and search
  // it.
  uint32_t start = non_null_->CountSetBits(in.start);
  uint32_t end = non_null_->CountSetBits(in.end);
  BitVector res = ReconcileStorageResult(
      op, *non_null_, storage_->Search(op, sql_val, Range(start, end)), in);

  PERFETTO_DCHECK(res.size() == in.end);
  return RangeOrBitVector(std::move(res));
}

RangeOrBitVector NullOverlay::IndexSearch(FilterOp op,
                                          SqlValue sql_val,
                                          uint32_t* indices,
                                          uint32_t indices_size,
                                          bool sorted) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "NullOverlay::IndexSearch");

  if (op == FilterOp::kIsNull) {
    switch (storage_->ValidateSearchConstraints(sql_val, op)) {
      case SearchValidationResult::kNoData: {
        BitVector::Builder null_indices(indices_size);
        for (uint32_t* it = indices; it != indices + indices_size; it++) {
          null_indices.Append(!non_null_->IsSet(*it));
        }
        // There is no need to search in underlying storage. We should just
        // check if the index is set in |non_null_|.
        return RangeOrBitVector(std::move(null_indices).Build());
      }
      case SearchValidationResult::kAllData:
        return RangeOrBitVector(Range(0, indices_size));
      case SearchValidationResult::kOk:
        break;
    }
  }

  BitVector::Builder storage_non_null(indices_size);
  std::vector<uint32_t> storage_iv;
  storage_iv.reserve(indices_size);
  for (uint32_t* it = indices; it != indices + indices_size; it++) {
    bool is_non_null = non_null_->IsSet(*it);
    if (is_non_null) {
      storage_iv.push_back(non_null_->CountSetBits(*it));
    }
    storage_non_null.Append(is_non_null);
  }
  RangeOrBitVector range_or_bv =
      storage_->IndexSearch(op, sql_val, storage_iv.data(),
                            static_cast<uint32_t>(storage_iv.size()), sorted);
  BitVector res =
      ReconcileStorageResult(op, std::move(storage_non_null).Build(),
                             std::move(range_or_bv), Range(0, indices_size));

  PERFETTO_DCHECK(res.size() == indices_size);
  return RangeOrBitVector(std::move(res));
}

void NullOverlay::StableSort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_FATAL("Not implemented");
}

void NullOverlay::Sort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_FATAL("Not implemented");
}

void NullOverlay::Serialize(StorageProto* storage) const {
  auto* null_overlay = storage->set_null_overlay();
  non_null_->Serialize(null_overlay->set_bit_vector());
  storage_->Serialize(null_overlay->set_storage());
}

}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
