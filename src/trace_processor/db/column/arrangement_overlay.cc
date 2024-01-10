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
#include <vector>

#include "protos/perfetto/trace_processor/serialization.pbzero.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/tp_metatrace.h"

namespace perfetto {
namespace trace_processor {
namespace column {
namespace {}  // namespace

ArrangementOverlay::ArrangementOverlay(std::unique_ptr<Column> inner,
                                       const std::vector<uint32_t>* arrangement)
    : inner_(std::move(inner)), arrangement_(arrangement) {
  PERFETTO_DCHECK(*std::max_element(arrangement->begin(), arrangement->end()) <=
                  inner_->size());
}

SearchValidationResult ArrangementOverlay::ValidateSearchConstraints(
    SqlValue sql_val,
    FilterOp op) const {
  return inner_->ValidateSearchConstraints(sql_val, op);
}

RangeOrBitVector ArrangementOverlay::Search(FilterOp op,
                                            SqlValue sql_val,
                                            Range in) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "ArrangementOverlay::Search");

  const auto& arrangement = *arrangement_;
  PERFETTO_DCHECK(in.end <= arrangement.size());
  const auto [min_i, max_i] =
      std::minmax_element(arrangement.begin() + static_cast<int32_t>(in.start),
                          arrangement.begin() + static_cast<int32_t>(in.end));

  auto storage_result = inner_->Search(op, sql_val, Range(*min_i, *max_i + 1));
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

RangeOrBitVector ArrangementOverlay::IndexSearch(FilterOp op,
                                                 SqlValue sql_val,
                                                 uint32_t* indices,
                                                 uint32_t indices_size,
                                                 bool sorted) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "ArrangementOverlay::IndexSearch");

  std::vector<uint32_t> storage_iv;
  for (uint32_t* it = indices; it != indices + indices_size; ++it) {
    storage_iv.push_back((*arrangement_)[*it]);
  }
  return inner_->IndexSearch(op, sql_val, storage_iv.data(),
                             static_cast<uint32_t>(storage_iv.size()), sorted);
}

void ArrangementOverlay::StableSort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_FATAL("Not implemented");
}

void ArrangementOverlay::Sort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_FATAL("Not implemented");
}

void ArrangementOverlay::Serialize(StorageProto* storage) const {
  auto* arrangement_overlay = storage->set_arrangement_overlay();
  arrangement_overlay->set_values(
      reinterpret_cast<const uint8_t*>(arrangement_->data()),
      sizeof(uint32_t) * arrangement_->size());
  inner_->Serialize(arrangement_overlay->set_storage());
}

}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
