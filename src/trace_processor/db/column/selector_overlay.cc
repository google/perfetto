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

#include "src/trace_processor/db/column/selector_overlay.h"

#include "protos/perfetto/trace_processor/serialization.pbzero.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/tp_metatrace.h"

namespace perfetto {
namespace trace_processor {
namespace column {

SelectorOverlay::SelectorOverlay(std::unique_ptr<Column> inner,
                                 const BitVector* selector)
    : inner_(std::move(inner)), selector_(selector) {}

SearchValidationResult SelectorOverlay::ValidateSearchConstraints(
    SqlValue sql_val,
    FilterOp op) const {
  return inner_->ValidateSearchConstraints(sql_val, op);
}

RangeOrBitVector SelectorOverlay::Search(FilterOp op,
                                         SqlValue sql_val,
                                         Range in) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "SelectorOverlay::Search");

  // Figure out the bounds of the indices in the underlying storage and search
  // it.
  uint32_t start_idx = selector_->IndexOfNthSet(in.start);
  uint32_t end_idx = selector_->IndexOfNthSet(in.end - 1) + 1;

  auto storage_result = inner_->Search(op, sql_val, Range(start_idx, end_idx));
  if (storage_result.IsRange()) {
    Range storage_range = std::move(storage_result).TakeIfRange();
    uint32_t out_start = selector_->CountSetBits(storage_range.start);
    uint32_t out_end = selector_->CountSetBits(storage_range.end);
    return RangeOrBitVector(Range(out_start, out_end));
  }

  BitVector storage_bitvector = std::move(storage_result).TakeIfBitVector();
  PERFETTO_DCHECK(storage_bitvector.size() <= selector_->size());

  // TODO(b/283763282): implement ParallelExtractBits to optimize this
  // operation.
  BitVector::Builder res(in.end);
  for (auto it = selector_->IterateSetBits();
       it && it.index() < storage_bitvector.size(); it.Next()) {
    res.Append(storage_bitvector.IsSet(it.index()));
  }
  return RangeOrBitVector(std::move(res).Build());
}

RangeOrBitVector SelectorOverlay::IndexSearch(FilterOp op,
                                              SqlValue sql_val,
                                              Indices indices) const {
  PERFETTO_DCHECK(
      indices.size == 0 ||
      *std::max_element(indices.data, indices.data + indices.size) <=
          selector_->size());
  // TODO(b/307482437): Use OrderedIndexSearch if arrangement orders storage.

  PERFETTO_TP_TRACE(metatrace::Category::DB, "SelectorOverlay::IndexSearch");

  // To go from TableIndexVector to StorageIndexVector we need to find index in
  // |selector_| by looking only into set bits.
  std::vector<uint32_t> storage_iv(indices.size);
  for (uint32_t i = 0; i < indices.size; ++i) {
    storage_iv[i] = selector_->IndexOfNthSet(indices.data[i]);
  }
  return inner_->IndexSearch(
      op, sql_val,
      Indices{storage_iv.data(), static_cast<uint32_t>(storage_iv.size()),
              indices.state});
}

Range SelectorOverlay::OrderedIndexSearch(FilterOp op,
                                          SqlValue sql_val,
                                          Indices indices) const {
  // To go from TableIndexVector to StorageIndexVector we need to find index in
  // |selector_| by looking only into set bits.
  std::vector<uint32_t> inner_indices(indices.size);
  for (uint32_t i = 0; i < indices.size; ++i) {
    inner_indices[i] = selector_->IndexOfNthSet(indices.data[i]);
  }
  return inner_->OrderedIndexSearch(
      op, sql_val,
      Indices{inner_indices.data(), static_cast<uint32_t>(inner_indices.size()),
              indices.state});
}

void SelectorOverlay::StableSort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_FATAL("Not implemented");
}

void SelectorOverlay::Sort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_FATAL("Not implemented");
}

void SelectorOverlay::Serialize(StorageProto* storage) const {
  auto* selector_overlay = storage->set_selector_overlay();
  inner_->Serialize(selector_overlay->set_storage());
  selector_->Serialize(selector_overlay->set_bit_vector());
}

}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
