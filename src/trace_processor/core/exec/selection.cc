/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/core/exec/selection.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/span.h"

#if PERFETTO_BUILDFLAG(PERFETTO_X64_CPU_OPT)
#include <immintrin.h>
#endif

namespace perfetto::trace_processor::core::exec {
namespace {

#if PERFETTO_BUILDFLAG(PERFETTO_X64_CPU_OPT)

// Compares eight values to `value` and returns a bitmask with bit i set if
// `values[i] == value`.
PERFETTO_ALWAYS_INLINE uint32_t EqualEight(const uint32_t* values,
                                           uint32_t value) {
  __m256i eight = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(values));
  __m256i equal =
      _mm256_cmpeq_epi32(eight, _mm256_set1_epi32(static_cast<int>(value)));
  return static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(equal)));
}

// For each 4-bit mask, the positions of its set bits, lowest first, packed one
// per byte. For example, mask 0b1010 maps to bytes {1, 3}.
constexpr uint32_t kPositions[16] = {
    0x00000000, 0x00000000, 0x00000001, 0x00000100, 0x00000002, 0x00000200,
    0x00000201, 0x00020100, 0x00000003, 0x00000300, 0x00000301, 0x00030100,
    0x00000302, 0x00030200, 0x00030201, 0x03020100};

// Writes `first + i` to `out` for each bit i set in the 8-bit mask `bits`,
// lowest first. This is the left-packing trick from Peter Cordes' Stack
// Overflow answer, but with a lookup table instead of BMI2's pdep/pext, which
// are slow on older AMD CPUs. Each half of the mask is looked up separately,
// and the high half's positions are shifted to land after the low half's.
// Always writes eight entries; only the first popcount(bits) are meaningful.
PERFETTO_ALWAYS_INLINE void WriteEight(uint32_t bits,
                                       uint32_t first,
                                       uint16_t* out) {
  uint32_t low = bits & 0xf;
  uint64_t at = kPositions[low] | uint64_t{kPositions[bits >> 4] + 0x04040404u}
                                      << (8 * PERFETTO_POPCOUNT(low));
  __m128i rows = _mm_cvtepu8_epi16(_mm_cvtsi64_si128(static_cast<int64_t>(at)));
  rows = _mm_add_epi16(rows, _mm_set1_epi16(static_cast<int16_t>(first)));
  _mm_storeu_si128(reinterpret_cast<__m128i*>(out), rows);
}

#endif  // PERFETTO_BUILDFLAG(PERFETTO_X64_CPU_OPT)

}  // namespace

void Selection::Keep(base::Span<const uint32_t> positions) {
  auto count = static_cast<uint32_t>(positions.size());
  PERFETTO_DCHECK(count <= size_);
  if (count == size_) {
    // Increasing, they can only be every position.
    return;
  }
  if (count == 0 || positions.b[count - 1] == count - 1) {
    // Increasing and ending at count - 1, they are the first `count`: the
    // rows kept stay as they are, a prefix if they were one, so readers keep
    // their contiguous paths.
    size_ = count;
    return;
  }
  if (prefix_) {
    for (uint32_t i = 0; i < count; ++i) {
      indices_[i] = static_cast<uint16_t>(positions.b[i]);
    }
  } else {
    // Each index is written at or before where it is read from.
    for (uint32_t i = 0; i < count; ++i) {
      PERFETTO_DCHECK(positions.b[i] < size_ &&
                      (i == 0 || positions.b[i - 1] < positions.b[i]));
      indices_[i] = indices_[positions.b[i]];
    }
  }
  size_ = count;
  prefix_ = false;
}

void Selection::KeepRows(base::Span<const uint32_t> rows) {
  auto count = static_cast<uint32_t>(rows.size());
  PERFETTO_DCHECK(count <= size_);
  if (count == size_) {
    // Kept already, they can only be every row kept.
    return;
  }
  size_ = count;
  if (prefix_ && (count == 0 || rows.b[count - 1] == count - 1)) {
    // Kept in order from a prefix, and ending at count - 1, they are the
    // first `count`.
    return;
  }
  Reorder(rows);
}

void Selection::Reorder(base::Span<const uint32_t> rows) {
  auto count = static_cast<uint32_t>(rows.size());
  PERFETTO_DCHECK(count <= size_);
  bool prefix = true;
  for (uint32_t i = 0; i < count; ++i) {
    PERFETTO_DCHECK(rows.b[i] < rows_);
    prefix &= rows.b[i] == i;
    indices_[i] = static_cast<uint16_t>(rows.b[i]);
  }
  size_ = count;
  prefix_ = prefix;
}

void Selection::KeepMatching(const uint32_t* values,
                             uint32_t value,
                             bool equal) {
  uint32_t n = 0;
  if (prefix_) {
    uint32_t i = 0;
#if PERFETTO_BUILDFLAG(PERFETTO_X64_CPU_OPT)
    // Eight rows at a time, as pipitdb's keep_prefix does. Writing never gets
    // ahead of reading, so it can't run past size_.
    uint32_t flip = equal ? 0 : 0xff;
    for (; i + 8 <= size_; i += 8) {
      uint32_t bits = EqualEight(values + i, value) ^ flip;
      WriteEight(bits, i, indices_ + n);
      n += static_cast<uint32_t>(PERFETTO_POPCOUNT(bits));
    }
#endif
    for (; i < size_; ++i) {
      indices_[n] = static_cast<uint16_t>(i);
      n += (values[i] == value) == equal;
    }
    // The kept rows are increasing. If the last is n - 1, they must be exactly
    // rows 0 to n - 1, which is still a prefix.
    prefix_ = n == 0 || indices_[n - 1] == n - 1;
  } else {
    for (uint32_t i = 0; i < size_; ++i) {
      uint16_t row = indices_[i];
      indices_[n] = row;
      n += (values[row] == value) == equal;
    }
  }
  size_ = n;
}

void Selection::Reverse() {
  if (size_ <= 1) {
    return;
  }
  if (prefix_) {
    for (uint32_t i = 0; i < size_; ++i) {
      indices_[i] = static_cast<uint16_t>(size_ - 1 - i);
    }
    prefix_ = false;
  } else {
    std::reverse(indices_, indices_ + size_);
  }
}

void Selection::KeepRange(uint32_t first, uint32_t count) {
  PERFETTO_DCHECK(first + count <= size_);
  if (first != 0) {
    if (prefix_) {
      for (uint32_t i = 0; i < count; ++i) {
        indices_[i] = static_cast<uint16_t>(first + i);
      }
    } else {
      memmove(indices_, indices_ + first, count * sizeof(indices_[0]));
    }
    prefix_ = false;
  }
  size_ = count;
}

void Selection::CopyFrom(const Selection& other) {
  rows_ = other.rows_;
  size_ = other.size_;
  prefix_ = other.prefix_;
  if (!prefix_) {
    memcpy(indices_, other.indices_, size_ * sizeof(indices_[0]));
  }
}

}  // namespace perfetto::trace_processor::core::exec
