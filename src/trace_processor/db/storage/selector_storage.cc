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

#include "src/trace_processor/db/storage/selector_storage.h"

#include "protos/perfetto/trace_processor/serialization.pbzero.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/storage/types.h"
#include "src/trace_processor/tp_metatrace.h"

namespace perfetto {
namespace trace_processor {
namespace storage {

using Range = RowMap::Range;

SelectorStorage::SelectorStorage(std::unique_ptr<Storage> inner,
                                 const BitVector* selector)
    : inner_(std::move(inner)), selector_(selector) {}

RangeOrBitVector SelectorStorage::Search(FilterOp op,
                                         SqlValue sql_val,
                                         RowMap::Range in) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "SelectorStorage::Search");

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

RangeOrBitVector SelectorStorage::IndexSearch(FilterOp op,
                                              SqlValue sql_val,
                                              uint32_t* indices,
                                              uint32_t indices_size,
                                              bool sorted) const {
  PERFETTO_DCHECK(indices_size == 0 ||
                  *std::max_element(indices, indices + indices_size) <=
                      selector_->size());

  PERFETTO_TP_TRACE(metatrace::Category::DB, "SelectorStorage::IndexSearch");

  // To go from TableIndexVector to StorageIndexVector we need to find index in
  // |selector_| by looking only into set bits.
  std::vector<uint32_t> storage_iv;
  storage_iv.reserve(indices_size);
  for (const uint32_t* it = indices; it != indices + indices_size; ++it) {
    storage_iv.push_back(selector_->IndexOfNthSet(*it));
  }
  return inner_->IndexSearch(op, sql_val, storage_iv.data(),
                             static_cast<uint32_t>(storage_iv.size()), sorted);
}

void SelectorStorage::StableSort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_FATAL("Not implemented");
}

void SelectorStorage::Sort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_FATAL("Not implemented");
}

void SelectorStorage::Serialize(StorageProto* storage) const {
  auto* selector_storage = storage->set_selector_storage();
  inner_->Serialize(selector_storage->set_storage());
  selector_->Serialize(selector_storage->set_bit_vector());
}

}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
