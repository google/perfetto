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

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/tp_metatrace.h"

#include "protos/perfetto/trace_processor/metatrace_categories.pbzero.h"
#include "protos/perfetto/trace_processor/serialization.pbzero.h"

namespace perfetto::trace_processor::column {
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

SingleSearchResult NullOverlay::ChainImpl::SingleSearch(FilterOp op,
                                                        SqlValue sql_val,
                                                        uint32_t index) const {
  switch (op) {
    case FilterOp::kIsNull:
      return non_null_->IsSet(index)
                 ? inner_->SingleSearch(op, sql_val,
                                        non_null_->CountSetBits(index))
                 : SingleSearchResult::kMatch;
    case FilterOp::kIsNotNull:
    case FilterOp::kEq:
    case FilterOp::kGe:
    case FilterOp::kGt:
    case FilterOp::kLt:
    case FilterOp::kLe:
    case FilterOp::kNe:
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      return non_null_->IsSet(index)
                 ? inner_->SingleSearch(op, sql_val,
                                        non_null_->CountSetBits(index))
                 : SingleSearchResult::kNoMatch;
  }
  PERFETTO_FATAL("For GCC");
}

UniqueSearchResult NullOverlay::ChainImpl::UniqueSearch(FilterOp op,
                                                        SqlValue sql_val,
                                                        uint32_t* index) const {
  switch (inner_->UniqueSearch(op, sql_val, index)) {
    case UniqueSearchResult::kMatch:
      if (*index >= non_null_->CountSetBits()) {
        return UniqueSearchResult::kNoMatch;
      }
      *index = non_null_->IndexOfNthSet(*index);
      return UniqueSearchResult::kMatch;
    case UniqueSearchResult::kNoMatch:
      return UniqueSearchResult::kNoMatch;
    case UniqueSearchResult::kNeedsFullSearch:
      return UniqueSearchResult::kNeedsFullSearch;
  }
  PERFETTO_FATAL("For GCC");
}

NullOverlay::ChainImpl::ChainImpl(std::unique_ptr<DataLayerChain> innner,
                                  const BitVector* non_null)
    : inner_(std::move(innner)), non_null_(non_null) {
  PERFETTO_DCHECK(non_null_->CountSetBits() <= inner_->size());
}

SearchValidationResult NullOverlay::ChainImpl::ValidateSearchConstraints(
    FilterOp op,
    SqlValue sql_val) const {
  if (op == FilterOp::kIsNull) {
    return SearchValidationResult::kOk;
  }
  return inner_->ValidateSearchConstraints(op, sql_val);
}

RangeOrBitVector NullOverlay::ChainImpl::SearchValidated(FilterOp op,
                                                         SqlValue sql_val,
                                                         Range in) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "NullOverlay::ChainImpl::Search");

  if (op == FilterOp::kIsNull) {
    switch (inner_->ValidateSearchConstraints(op, sql_val)) {
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
      op, *non_null_, inner_->SearchValidated(op, sql_val, Range(start, end)),
      in);

  PERFETTO_DCHECK(res.size() == in.end);
  return RangeOrBitVector(std::move(res));
}

RangeOrBitVector NullOverlay::ChainImpl::IndexSearchValidated(
    FilterOp op,
    SqlValue sql_val,
    Indices indices) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB,
                    "NullOverlay::ChainImpl::IndexSearch");

  if (op == FilterOp::kIsNull) {
    switch (inner_->ValidateSearchConstraints(op, sql_val)) {
      case SearchValidationResult::kNoData: {
        BitVector::Builder null_indices(indices.size);
        for (const uint32_t* it = indices.data;
             it != indices.data + indices.size; it++) {
          null_indices.Append(!non_null_->IsSet(*it));
        }
        // There is no need to search in underlying storage. We should just
        // check if the index is set in |non_null_|.
        return RangeOrBitVector(std::move(null_indices).Build());
      }
      case SearchValidationResult::kAllData:
        return RangeOrBitVector(Range(0, indices.size));
      case SearchValidationResult::kOk:
        break;
    }
  }

  BitVector::Builder storage_non_null(indices.size);
  std::vector<uint32_t> storage_iv;
  storage_iv.reserve(indices.size);
  for (const uint32_t* it = indices.data; it != indices.data + indices.size;
       it++) {
    bool is_non_null = non_null_->IsSet(*it);
    if (is_non_null) {
      storage_iv.push_back(non_null_->CountSetBits(*it));
    }
    storage_non_null.Append(is_non_null);
  }
  RangeOrBitVector range_or_bv = inner_->IndexSearchValidated(
      op, sql_val,
      Indices{storage_iv.data(), static_cast<uint32_t>(storage_iv.size()),
              indices.state});
  BitVector res =
      ReconcileStorageResult(op, std::move(storage_non_null).Build(),
                             std::move(range_or_bv), Range(0, indices.size));

  PERFETTO_DCHECK(res.size() == indices.size);
  return RangeOrBitVector(std::move(res));
}

Range NullOverlay::ChainImpl::OrderedIndexSearchValidated(
    FilterOp op,
    SqlValue sql_val,
    Indices indices) const {
  // For NOT EQUAL the translation or results from EQUAL needs to be done by the
  // caller.
  PERFETTO_CHECK(op != FilterOp::kNe);

  PERFETTO_TP_TRACE(metatrace::Category::DB,
                    "NullOverlay::ChainImpl::OrderedIndexSearch");

  // We assume all NULLs are ordered to be in the front. We are looking for the
  // first index that points to non NULL value.
  const uint32_t* first_non_null =
      std::partition_point(indices.data, indices.data + indices.size,
                           [this](uint32_t i) { return !non_null_->IsSet(i); });
  auto non_null_offset =
      static_cast<uint32_t>(std::distance(indices.data, first_non_null));
  auto non_null_size = static_cast<uint32_t>(
      std::distance(first_non_null, indices.data + indices.size));

  if (op == FilterOp::kIsNull) {
    return {0, non_null_offset};
  }

  if (op == FilterOp::kIsNotNull) {
    switch (inner_->ValidateSearchConstraints(op, sql_val)) {
      case SearchValidationResult::kNoData:
        return {};
      case SearchValidationResult::kAllData:
        return {non_null_offset, indices.size};
      case SearchValidationResult::kOk:
        break;
    }
  }

  std::vector<uint32_t> storage_iv;
  storage_iv.reserve(non_null_size);
  for (const uint32_t* it = first_non_null;
       it != first_non_null + non_null_size; it++) {
    storage_iv.push_back(non_null_->CountSetBits(*it));
  }

  Range inner_range = inner_->OrderedIndexSearchValidated(
      op, sql_val, Indices{storage_iv.data(), non_null_size, indices.state});
  return {inner_range.start + non_null_offset,
          inner_range.end + non_null_offset};
}

void NullOverlay::ChainImpl::StableSort(SortToken* start,
                                        SortToken* end,
                                        SortDirection direction) const {
  SortToken* middle = std::stable_partition(
      start, end,
      [this](const SortToken& idx) { return !non_null_->IsSet(idx.index); });
  for (SortToken* it = middle; it != end; ++it) {
    it->index = non_null_->CountSetBits(it->index);
  }
  inner_->StableSort(middle, end, direction);
  if (direction == SortDirection::kDescending) {
    std::rotate(start, middle, end);
  }
}

void NullOverlay::ChainImpl::Serialize(StorageProto* storage) const {
  auto* null_storage = storage->set_null_overlay();
  non_null_->Serialize(null_storage->set_bit_vector());
  inner_->Serialize(null_storage->set_storage());
}

}  // namespace perfetto::trace_processor::column
