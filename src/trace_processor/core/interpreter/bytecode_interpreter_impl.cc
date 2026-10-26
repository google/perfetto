/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/core/interpreter/bytecode_interpreter_impl.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string_view>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/ops.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::interpreter::ops {

void LimitOffsetIndices(InterpreterState& state,
                        const struct LimitOffsetIndices& bytecode) {
  using B = struct LimitOffsetIndices;
  uint32_t offset_value = bytecode.arg<B::offset_value>();
  uint32_t limit_value = bytecode.arg<B::limit_value>();
  auto& span = state.ReadFromRegister(bytecode.arg<B::update_register>());

  // Apply offset
  auto original_size = static_cast<uint32_t>(span.size());
  uint32_t actual_offset = std::min(offset_value, original_size);
  span.b += actual_offset;

  // Apply limit
  auto size_after_offset = static_cast<uint32_t>(span.size());
  uint32_t actual_limit = std::min(limit_value, size_after_offset);
  span.e = span.b + actual_limit;
}

void CopySpanIntersectingRange(
    InterpreterState& state,
    const struct CopySpanIntersectingRange& bytecode) {
  using B = struct CopySpanIntersectingRange;
  const auto& source =
      state.ReadFromRegister(bytecode.arg<B::source_register>());
  const auto& source_range =
      state.ReadFromRegister(bytecode.arg<B::source_range_register>());
  auto& update = state.ReadFromRegister(bytecode.arg<B::update_register>());
  PERFETTO_DCHECK(source.size() <= update.size());
  update.e = filter::CopySpanIntersectingRange(
      Span<const uint32_t>(source.b, source.e), source_range, update.b);
}

void InitRankMap(InterpreterState& state, const struct InitRankMap& bytecode) {
  using B = struct InitRankMap;

  StringIdToRankMap* rank_map =
      state.MaybeReadFromRegister(bytecode.arg<B::dest_register>());
  if (rank_map) {
    rank_map->get()->Clear();
  } else {
    state.WriteToRegister(bytecode.arg<B::dest_register>(),
                          std::make_unique<StringRanks>());
  }
}

void FinalizeRanksInMap(InterpreterState& state,
                        const struct FinalizeRanksInMap& bytecode) {
  using B = struct FinalizeRanksInMap;
  StringIdToRankMap& rank_map_ptr =
      state.ReadFromRegister(bytecode.arg<B::update_register>());
  PERFETTO_DCHECK(rank_map_ptr);
  rank_map_ptr->Rank(*state.string_pool);
}

void Distinct(InterpreterState& state, const struct Distinct& bytecode) {
  using B = struct Distinct;
  auto& indices = state.ReadFromRegister(bytecode.arg<B::indices_register>());
  if (indices.empty()) {
    return;
  }
  const auto& buffer =
      state.ReadFromRegister(bytecode.arg<B::buffer_register>());
  uint32_t stride = bytecode.arg<B::total_row_stride>();
  core::ops::DistinctRows(buffer, stride, &indices);
}

void SortRowLayout(InterpreterState& state,
                   const struct SortRowLayout& bytecode) {
  using B = struct SortRowLayout;
  auto& indices = state.ReadFromRegister(bytecode.arg<B::indices_register>());
  // Single element is always sorted.
  if (indices.size() <= 1) {
    return;
  }
  const auto& buffer =
      state.ReadFromRegister(bytecode.arg<B::buffer_register>());
  uint32_t stride = bytecode.arg<B::total_row_stride>();
  core::ops::SortRowLayout(buffer, stride, &indices);
}

void TranslateSparseNullIndices(
    InterpreterState& state,
    const struct TranslateSparseNullIndices& bytecode) {
  using B = struct TranslateSparseNullIndices;
  const NullBitvector& nbv =
      state.ReadFromRegister(bytecode.arg<B::null_bv_register>());

  const auto& source =
      state.ReadFromRegister(bytecode.arg<B::source_register>());
  auto& update = state.ReadFromRegister(bytecode.arg<B::update_register>());
  PERFETTO_DCHECK(source.size() <= update.size());

  uint32_t* out = update.b;
  for (uint32_t* it = source.b; it != source.e; ++it, ++out) {
    uint32_t s = *it;
    *out = static_cast<uint32_t>(nbv.popcount[s / 64] +
                                 nbv.bv->count_set_bits_until_in_word(s));
  }
  update.e = out;
}

void AllocateRowLayoutBuffer(InterpreterState& state,
                             const struct AllocateRowLayoutBuffer& bytecode) {
  using B = struct AllocateRowLayoutBuffer;
  uint32_t size = bytecode.arg<B::buffer_size>();
  auto dest_reg = bytecode.arg<B::dest_buffer_register>();
  // Return early if buffer already allocated.
  if (state.MaybeReadFromRegister(dest_reg)) {
    return;
  }
  state.WriteToRegister(dest_reg, Slab<uint8_t>::Alloc(size));
}

void CollectIdIntoRankMap(InterpreterState& state,
                          const struct CollectIdIntoRankMap& bytecode) {
  using B = struct CollectIdIntoRankMap;

  StringIdToRankMap& rank_map_ptr =
      state.ReadFromRegister(bytecode.arg<B::rank_map_register>());
  PERFETTO_DCHECK(rank_map_ptr);
  auto& rank_map = *rank_map_ptr;

  const StringPool::Id* data = state.ReadStorageFromRegister<String>(
      bytecode.arg<B::storage_register>());
  const auto& source =
      state.ReadFromRegister(bytecode.arg<B::source_register>());
  for (const uint32_t* it = source.b; it != source.e; ++it) {
    rank_map.Add(data[*it]);
  }
}

void StrideTranslateAndCopySparseNullIndices(
    InterpreterState& state,
    const struct StrideTranslateAndCopySparseNullIndices& bytecode) {
  using B = struct StrideTranslateAndCopySparseNullIndices;
  const NullBitvector& nbv =
      state.ReadFromRegister(bytecode.arg<B::null_bv_register>());

  auto& update = state.ReadFromRegister(bytecode.arg<B::update_register>());
  uint32_t stride = bytecode.arg<B::stride>();
  uint32_t offset = bytecode.arg<B::offset>();
  for (uint32_t* it = update.b; it != update.e; it += stride) {
    uint32_t index = *it;
    if (nbv.bv->is_set(index)) {
      it[offset] =
          static_cast<uint32_t>(nbv.popcount[index / 64] +
                                nbv.bv->count_set_bits_until_in_word(index));
    } else {
      it[offset] = std::numeric_limits<uint32_t>::max();
    }
  }
}

void StrideCopyDenseNullIndices(
    InterpreterState& state,
    const struct StrideCopyDenseNullIndices& bytecode) {
  using B = struct StrideCopyDenseNullIndices;
  const NullBitvector& nbv =
      state.ReadFromRegister(bytecode.arg<B::null_bv_register>());

  auto& update = state.ReadFromRegister(bytecode.arg<B::update_register>());
  uint32_t stride = bytecode.arg<B::stride>();
  uint32_t offset = bytecode.arg<B::offset>();
  for (uint32_t* it = update.b; it != update.e; it += stride) {
    it[offset] =
        nbv.bv->is_set(*it) ? *it : std::numeric_limits<uint32_t>::max();
  }
}

}  // namespace perfetto::trace_processor::core::interpreter::ops
