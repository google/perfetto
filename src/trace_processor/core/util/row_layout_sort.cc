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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "perfetto/base/endian.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/small_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/ops.h"
#include "src/trace_processor/core/util/sort.h"
#include "src/trace_processor/core/util/span.h"

// SortRowLayout sorts on the bytes which differ between rows, the first 8 of
// them packed into one integer per row so that sorting does not read the rows
// again. As in DuckDB, rows which agree on those 8 bytes are then sorted on the
// whole row.

namespace perfetto::trace_processor::core::ops {
namespace {

// Up to this many ascending runs, of this many rows on average, are merged
// rather than sorted.
constexpr size_t kMaxMergedRuns = 32;
constexpr size_t kRowsPerMergedRun = 256;

constexpr uint32_t kMaxKeyBytes = 8;

struct Rows {
  const uint8_t* operator[](uint32_t row) const {
    return data + (size_t{row} * stride);
  }
  const uint8_t* data;
  uint32_t stride;
  uint32_t count;
};

// A row's first 8 varying bytes, big-endian so they compare as the row does,
// its position and its index. Ties break on position, so even an unstable
// sort is stable.
struct Token {
  uint64_t key;
  uint32_t position;
  uint32_t index;
};

// Buffers one sort works in.
struct Scratch {
  FlexVector<Token> tokens;
  FlexVector<Token> spare;
  std::vector<uint64_t> first_words;
  std::vector<uint64_t> differing_bits;
  std::vector<uint8_t> differs;
  std::vector<uint32_t> varying;
};

// Resizes `v` to `size` elements and returns them.
template <typename T>
T* Resize(FlexVector<T>& v, size_t size) {
  v.resize(size);
  return v.data();
}

// memcmp, as big-endian words when the rows are at least 8 bytes.
int CompareRows(const uint8_t* a, const uint8_t* b, uint32_t stride) {
  if (stride < 8) {
    return memcmp(a, b, stride);
  }
  for (uint32_t at = 0;; at = std::min(at + 8, stride - 8)) {
    uint64_t x;
    uint64_t y;
    memcpy(&x, a + at, 8);
    memcpy(&y, b + at, 8);
    if (x != y) {
      return base::BE64ToHost(x) < base::BE64ToHost(y) ? -1 : 1;
    }
    if (at == stride - 8) {
      return 0;
    }
  }
}

struct ExistingOrder {
  bool sorted = false;
  bool strictly_descending = false;
  // Where each ascending run starts, then the row count, if there are few.
  base::SmallVector<uint32_t, kMaxMergedRuns + 2> runs;
};

// Stops as soon as the rows are neither in a few runs nor strictly descending,
// which for most input is within a few rows.
ExistingOrder FindExistingOrder(const Rows& rows) {
  size_t max_runs =
      std::min(kMaxMergedRuns, size_t{rows.count} / kRowsPerMergedRun);
  ExistingOrder order;
  order.runs.emplace_back(0u);
  bool few_runs = true;
  bool descending = true;
  const uint8_t* row = rows[0];
  for (uint32_t r = 1; r < rows.count && (few_runs || descending); ++r) {
    int cmp = CompareRows(row, row + rows.stride, rows.stride);
    row += rows.stride;
    descending &= cmp > 0;
    if (cmp > 0 && few_runs) {
      few_runs = order.runs.size() < max_runs;
      order.runs.emplace_back(r);
    }
  }
  order.sorted = few_runs && order.runs.size() == 1;
  order.strictly_descending = descending;
  if (few_runs) {
    order.runs.emplace_back(rows.count);
  } else {
    order.runs.clear();
  }
  return order;
}

// Sets `scratch.varying` to the positions of the bytes which differ between
// any two rows.
void FindVaryingBytes(const Rows& rows, Scratch& scratch) {
  std::vector<uint8_t>& differs = scratch.differs;
  differs.assign(rows.stride, 0);
  if (rows.stride >= 8) {
    // A word at a time, the last of which may overlap the one before it.
    uint32_t words = (rows.stride + 7) / 8;
    auto word_at = [&rows](uint32_t w) {
      return std::min(w * 8, rows.stride - 8);
    };
    std::vector<uint64_t>& first = scratch.first_words;
    std::vector<uint64_t>& diff = scratch.differing_bits;
    first.resize(words);
    diff.assign(words, 0);
    for (uint32_t w = 0; w < words; ++w) {
      memcpy(&first[w], rows[0] + word_at(w), 8);
    }
    for (uint32_t r = 0; r < rows.count; ++r) {
      for (uint32_t w = 0; w < words; ++w) {
        uint64_t word;
        memcpy(&word, rows[r] + word_at(w), 8);
        diff[w] |= word ^ first[w];
      }
    }
    for (uint32_t w = 0; w < words; ++w) {
      for (uint32_t b = 0; b < 8; ++b) {
        differs[word_at(w) + b] |= ((diff[w] >> (b * 8)) & 0xFF) != 0;
      }
    }
  } else {
    for (uint32_t r = 0; r < rows.count; ++r) {
      for (uint32_t b = 0; b < rows.stride; ++b) {
        differs[b] |= rows[r][b] != rows[0][b];
      }
    }
  }
  scratch.varying.clear();
  for (uint32_t b = 0; b < rows.stride; ++b) {
    if (differs[b]) {
      scratch.varying.push_back(b);
    }
  }
}

Token* PackKeys(const Rows& rows,
                uint32_t key_bytes,
                const uint32_t* indices,
                Scratch& scratch) {
  Token* tokens = Resize(scratch.tokens, rows.count);
  const uint32_t* varying = scratch.varying.data();
  for (uint32_t r = 0; r < rows.count; ++r) {
    const uint8_t* row = rows[r];
    uint64_t key = 0;
    for (uint32_t b = 0; b < key_bytes; ++b) {
      key = (key << 8) | row[varying[b]];
    }
    tokens[r] = {base::HostToBE64(key), r, indices[r]};
  }
  return tokens;
}

// Merges pairs of runs, earlier run first so ties keep their order, until one
// is left. Returns whichever of `tokens` and `scratch` holds the result.
Token* MergeRuns(Token* tokens,
                 Token* scratch,
                 base::SmallVector<uint32_t, kMaxMergedRuns + 2> runs) {
  auto by_key = [](const Token& a, const Token& b) {
    return base::BE64ToHost(a.key) < base::BE64ToHost(b.key);
  };
  Token* source = tokens;
  Token* dest = scratch;
  while (runs.size() > 2) {
    size_t kept = 0;
    size_t r = 0;
    for (; r + 2 < runs.size(); r += 2) {
      std::merge(source + runs[r], source + runs[r + 1], source + runs[r + 1],
                 source + runs[r + 2], dest + runs[r], by_key);
      runs[kept++] = runs[r];
    }
    if (r + 1 < runs.size()) {
      memcpy(dest + runs[r], source + runs[r],
             size_t{runs[r + 1] - runs[r]} * sizeof(Token));
      runs[kept++] = runs[r];
    }
    runs[kept++] = runs.back();
    while (runs.size() > kept) {
      runs.pop_back();
    }
    std::swap(source, dest);
  }
  return source;
}

// Sorts each run of tokens with equal keys on their whole rows.
void SortTiesOnWholeRows(Token* tokens, const Rows& rows) {
  auto by_row = [&rows](const Token& a, const Token& b) {
    int cmp = CompareRows(rows[a.position], rows[b.position], rows.stride);
    return cmp != 0 ? cmp < 0 : a.position < b.position;
  };
  for (uint32_t i = 0; i < rows.count;) {
    uint32_t j = i + 1;
    while (j < rows.count && tokens[j].key == tokens[i].key) {
      ++j;
    }
    if (j - i > 1) {
      std::sort(tokens + i, tokens + j, by_row);
    }
    i = j;
  }
}

}  // namespace

void SortRowLayout(Span<const uint8_t> row_layout,
                   uint32_t row_stride,
                   Span<uint32_t>* indices) {
  PERFETTO_DCHECK(indices);
  auto count = static_cast<uint32_t>(indices->size());
  if (count <= 1) {
    return;
  }
  PERFETTO_DCHECK(row_layout.size() >= size_t{count} * row_stride);
  Rows rows{row_layout.b, row_stride, count};

  ExistingOrder order = FindExistingOrder(rows);
  if (order.sorted) {
    return;
  }
  if (order.strictly_descending) {
    // No two rows are equal, so reversing keeps ties in order.
    std::reverse(indices->b, indices->e);
    return;
  }

  Scratch scratch;
  FindVaryingBytes(rows, scratch);
  auto key_bytes = static_cast<uint32_t>(
      std::min<size_t>(scratch.varying.size(), kMaxKeyBytes));
  Token* tokens = PackKeys(rows, key_bytes, indices->b, scratch);
  Token* sorted = tokens;
  if (!order.runs.empty()) {
    sorted = MergeRuns(tokens, Resize(scratch.spare, count), order.runs);
  } else {
    sorted = StableSortByKey(
        tokens, tokens + count, Resize(scratch.spare, count), key_bytes * 8,
        [](const Token& t) { return base::BE64ToHost(t.key); },
        [](const Token& t) { return t.position; });
  }
  if (scratch.varying.size() > kMaxKeyBytes) {
    SortTiesOnWholeRows(sorted, rows);
  }
  for (uint32_t r = 0; r < count; ++r) {
    indices->b[r] = sorted[r].index;
  }
}

}  // namespace perfetto::trace_processor::core::ops
