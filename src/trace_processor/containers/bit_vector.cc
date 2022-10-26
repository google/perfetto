/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/containers/bit_vector.h"

#include <limits>

#include "src/trace_processor/containers/bit_vector_iterators.h"

#if PERFETTO_BUILDFLAG(PERFETTO_X64_CPU_OPT)
#include <immintrin.h>
#endif

namespace perfetto {
namespace trace_processor {
namespace {

// This function implements the PDEP instruction in x64 as a loop.
// See https://www.felixcloutier.com/x86/pdep for details on what PDEP does.
//
// Unfortunately, as we're emulating this in software, it scales with the number
// of set bits in |mask| rather than being a constant time instruction:
// therefore, this should be avoided where real instructions are available.
uint64_t PdepSlow(uint64_t word, uint64_t mask) {
  if (word == 0 || mask == std::numeric_limits<uint64_t>::max())
    return word;

  // This algorithm is for calculating PDEP was found to be the fastest "simple"
  // one among those tested when writing this function.
  uint64_t result = 0;
  for (uint64_t bb = 1; mask; bb += bb) {
    if (word & bb) {
      // MSVC doesn't like -mask so work around this by doing 0 - mask.
      result |= mask & (0ull - mask);
    }
    mask &= mask - 1;
  }
  return result;
}

// See |PdepSlow| for information on PDEP.
uint64_t Pdep(uint64_t word, uint64_t mask) {
#if PERFETTO_BUILDFLAG(PERFETTO_X64_CPU_OPT)
  base::ignore_result(PdepSlow);
  return _pdep_u64(word, mask);
#else
  return PdepSlow(word, mask);
#endif
}

}  // namespace

BitVector::BitVector() = default;

BitVector::BitVector(std::initializer_list<bool> init) {
  for (bool x : init) {
    if (x) {
      AppendTrue();
    } else {
      AppendFalse();
    }
  }
}

BitVector::BitVector(uint32_t count, bool value) {
  Resize(count, value);
}

BitVector::BitVector(std::vector<Block> blocks,
                     std::vector<uint32_t> counts,
                     uint32_t size)
    : size_(size), counts_(std::move(counts)), blocks_(std::move(blocks)) {}

BitVector BitVector::Copy() const {
  return BitVector(blocks_, counts_, size_);
}

BitVector::AllBitsIterator BitVector::IterateAllBits() const {
  return AllBitsIterator(this);
}

BitVector::SetBitsIterator BitVector::IterateSetBits() const {
  return SetBitsIterator(this);
}

void BitVector::UpdateSetBits(const BitVector& update) {
  static_assert(sizeof(Block) == Block::kWords * sizeof(uint64_t),
                "Block must just consist of words.");
  PERFETTO_DCHECK(update.size() <= CountSetBits());

  // Get the start and end ptrs for the current bitvector.
  // Safe because of the static_assert above.
  auto* ptr = reinterpret_cast<uint64_t*>(blocks_.data());
  const uint64_t* ptr_end = ptr + WordCeil(size());

  // Get the start and end ptrs for the current bitvector.
  // Safe because of the static_assert above.
  auto* update_ptr = reinterpret_cast<const uint64_t*>(update.blocks_.data());
  const uint64_t* update_ptr_end = update_ptr + WordCeil(update.size());

  // |update_unused_bits| contains |unused_bits_count| bits at the bottom
  // which indicates the how the next |unused_bits_count| set bits in |this|
  // should be changed. This is necessary because word boundaries in |this| will
  // almost always *not* match the word boundaries in |update|.
  uint64_t update_unused_bits = 0;
  uint8_t unused_bits_count = 0;

  // The basic premise of this loop is, for each word in |this| we find the
  // enough bits from |update| to cover every set bit in the word. We then use
  // the PDEP x64 instruction (or equivalent instructions/software emulation) to
  // update the word and store it back in |this|.
  for (; ptr != ptr_end; ++ptr) {
    uint64_t current = *ptr;

    // If the current value is all zeros, there's nothing to update.
    if (PERFETTO_UNLIKELY(current == 0))
      continue;

    uint8_t popcount = static_cast<uint8_t>(PERFETTO_POPCOUNT(current));
    PERFETTO_DCHECK(popcount >= 1);

    // Check if we have enough unused bits from the previous iteration - if so,
    // we don't need to read anything from |update|.
    uint64_t update_for_current = update_unused_bits;
    if (unused_bits_count >= popcount) {
      // We have enough bits so just do the accounting to not reuse these bits
      // for the future.
      unused_bits_count -= popcount;
      update_unused_bits = popcount == 64 ? 0 : update_unused_bits >> popcount;
    } else {
      // We don't have enough bits so we need to read the next word of bits from
      // |current|.
      uint64_t next_update = update_ptr == update_ptr_end ? 0 : *update_ptr++;

      // Bitwise or |64 - unused_bits_count| bits from the bottom of
      // |next_update| to the top of |update_for_current|. Only |popcount| bits
      // will actually be used by PDEP but masking off the unused bits takes
      // *more* instructions than not doing anything.
      update_for_current |= next_update << unused_bits_count;

      // PDEP will use |popcount| bits from update: this means it will use
      // |unused_bits_count| from |update_for_current| and |popcount -
      // unused_bits_count| from |next_update|
      uint8_t used_next_bits = popcount - unused_bits_count;

      // Shift off any bits which will be used by current and store the
      // remainder for use in the next iteration.
      update_unused_bits =
          used_next_bits == 64 ? 0 : next_update >> used_next_bits;
      unused_bits_count = 64 - used_next_bits;
    }

    // We should never end up with more than 64 bits available.
    PERFETTO_CHECK(unused_bits_count <= 64);

    // PDEP precisely captures the notion of "updating set bits" for a single
    // word.
    *ptr = Pdep(update_for_current, current);
  }

  // We shouldn't have any non-zero unused bits and we should have consumed the
  // whole |update| bitvector. Note that we cannot really say anything about
  // |unused_bits_count| because it's possible for the above algorithm to use
  // some bits which are "past the end" of |update|; as long as these bits are
  // zero, it meets the pre-condition of this function.
  PERFETTO_DCHECK(update_unused_bits == 0);
  PERFETTO_DCHECK(update_ptr == update_ptr_end);

  for (uint32_t i = 0; i < counts_.size() - 1; ++i) {
    counts_[i + 1] = counts_[i] + blocks_[i].CountSetBits();
  }

  // After the loop, we should have precisely the same number of bits
  // set as |update|.
  PERFETTO_DCHECK(update.CountSetBits() == CountSetBits());
}

}  // namespace trace_processor
}  // namespace perfetto
