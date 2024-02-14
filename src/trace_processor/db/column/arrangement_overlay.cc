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

#include "src/trace_processor/db/column/arrangement_overlay.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/tp_metatrace.h"

#include "protos/perfetto/trace_processor/metatrace_categories.pbzero.h"
#include "protos/perfetto/trace_processor/serialization.pbzero.h"

namespace perfetto::trace_processor::column {

ArrangementOverlay::ArrangementOverlay(const std::vector<uint32_t>* arrangement,
                                       Indices::State arrangement_state)
    : arrangement_(arrangement), arrangement_state_(arrangement_state) {}

std::unique_ptr<DataLayerChain> ArrangementOverlay::MakeChain(
    std::unique_ptr<DataLayerChain> inner,
    ChainCreationArgs args) {
  return std::make_unique<ChainImpl>(std::move(inner), arrangement_,
                                     arrangement_state_,
                                     args.does_layer_order_chain_contents);
}

ArrangementOverlay::ChainImpl::ChainImpl(
    std::unique_ptr<DataLayerChain> inner,
    const std::vector<uint32_t>* arrangement,
    Indices::State arrangement_state,
    bool does_arrangement_order_storage)
    : inner_(std::move(inner)),
      arrangement_(arrangement),
      arrangement_state_(arrangement_state),
      does_arrangement_order_storage_(does_arrangement_order_storage) {
  PERFETTO_DCHECK(*std::max_element(arrangement->begin(), arrangement->end()) <=
                  inner_->size());
}

SingleSearchResult ArrangementOverlay::ChainImpl::SingleSearch(
    FilterOp op,
    SqlValue sql_val,
    uint32_t index) const {
  return inner_->SingleSearch(op, sql_val, (*arrangement_)[index]);
}

SearchValidationResult ArrangementOverlay::ChainImpl::ValidateSearchConstraints(
    FilterOp op,
    SqlValue value) const {
  return inner_->ValidateSearchConstraints(op, value);
}

RangeOrBitVector ArrangementOverlay::ChainImpl::SearchValidated(
    FilterOp op,
    SqlValue sql_val,
    Range in) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB,
                    "ArrangementOverlay::ChainImpl::Search");

  if (does_arrangement_order_storage_ && op != FilterOp::kGlob &&
      op != FilterOp::kRegex) {
    Range inner_res = inner_->OrderedIndexSearchValidated(
        op, sql_val,
        Indices{arrangement_->data() + in.start, in.size(),
                arrangement_state_});
    return RangeOrBitVector(
        Range(inner_res.start + in.start, inner_res.end + in.start));
  }

  const auto& arrangement = *arrangement_;
  PERFETTO_DCHECK(in.end <= arrangement.size());
  const auto [min_i, max_i] =
      std::minmax_element(arrangement.begin() + static_cast<int32_t>(in.start),
                          arrangement.begin() + static_cast<int32_t>(in.end));

  auto storage_result =
      inner_->SearchValidated(op, sql_val, Range(*min_i, *max_i + 1));
  BitVector::Builder builder(in.end, in.start);
  if (storage_result.IsRange()) {
    Range storage_range = std::move(storage_result).TakeIfRange();
    for (uint32_t i = in.start; i < in.end; ++i) {
      builder.Append(storage_range.Contains(arrangement[i]));
    }
  } else {
    BitVector storage_bitvector = std::move(storage_result).TakeIfBitVector();
    PERFETTO_DCHECK(storage_bitvector.size() == *max_i + 1);

    // After benchmarking, it turns out this complexity *is* actually worthwhile
    // and has a noticable impact on the performance of this function in real
    // world tables.

    // Fast path: we compare as many groups of 64 elements as we can.
    // This should be very easy for the compiler to auto-vectorize.
    const uint32_t* arrangement_idx = arrangement.data() + in.start;
    uint32_t fast_path_elements = builder.BitsInCompleteWordsUntilFull();
    for (uint32_t i = 0; i < fast_path_elements; i += BitVector::kBitsInWord) {
      uint64_t word = 0;
      // This part should be optimised by SIMD and is expected to be fast.
      for (uint32_t k = 0; k < BitVector::kBitsInWord; ++k, ++arrangement_idx) {
        bool comp_result = storage_bitvector.IsSet(*arrangement_idx);
        word |= static_cast<uint64_t>(comp_result) << k;
      }
      builder.AppendWord(word);
    }

    // Slow path: we compare <64 elements and append to fill the Builder.
    uint32_t back_elements = builder.BitsUntilFull();
    for (uint32_t i = 0; i < back_elements; ++i, ++arrangement_idx) {
      builder.Append(storage_bitvector.IsSet(*arrangement_idx));
    }
  }
  return RangeOrBitVector(std::move(builder).Build());
}

RangeOrBitVector ArrangementOverlay::ChainImpl::IndexSearchValidated(
    FilterOp op,
    SqlValue sql_val,
    Indices indices) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB,
                    "ArrangementOverlay::ChainImpl::IndexSearch");

  std::vector<uint32_t> storage_iv(indices.size);
  // Should be SIMD optimized.
  for (uint32_t i = 0; i < indices.size; ++i) {
    storage_iv[i] = (*arrangement_)[indices.data[i]];
  }

  // If both the arrangment passed indices are monotonic, we know that this
  // state was not lost.
  if (indices.state == Indices::State::kMonotonic) {
    return inner_->IndexSearchValidated(
        op, sql_val,
        Indices{storage_iv.data(), static_cast<uint32_t>(storage_iv.size()),
                arrangement_state_});
  }
  return inner_->IndexSearchValidated(
      op, sql_val,
      Indices{storage_iv.data(), static_cast<uint32_t>(storage_iv.size()),
              Indices::State::kNonmonotonic});
}

void ArrangementOverlay::ChainImpl::StableSort(SortToken* start,
                                               SortToken* end,
                                               SortDirection direction) const {
  for (SortToken* it = start; it != end; ++it) {
    it->index = (*arrangement_)[it->index];
  }
  inner_->StableSort(start, end, direction);
}

void ArrangementOverlay::ChainImpl::Serialize(StorageProto* storage) const {
  auto* arrangement_overlay = storage->set_arrangement_overlay();
  arrangement_overlay->set_values(
      reinterpret_cast<const uint8_t*>(arrangement_->data()),
      sizeof(uint32_t) * arrangement_->size());
  inner_->Serialize(arrangement_overlay->set_storage());
}

}  // namespace perfetto::trace_processor::column
