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
#include "src/trace_processor/core/util/span.h"
#include "src/trace_processor/util/sort.h"

// SortRowLayout sorts on the bytes which differ between rows, the first 8 of
// them packed into one integer per row so that sorting does not read the rows
// again. Rows which agree on those 8 bytes are then sorted on the next 8, and
// so on, with the same sort.

namespace perfetto::trace_processor::core::ops {
namespace {

// Up to this many ascending runs, of this many rows on average, are merged
// rather than sorted.
constexpr size_t kMaxMergedRuns = 32;
constexpr size_t kRowsPerMergedRun = 256;

constexpr uint32_t kMaxKeyBytes = 8;

struct Rows {
  const uint8_t* operator[](uint32_t row) const {
    return data + (static_cast<size_t>(row) * stride);
  }
  const uint8_t* data;
  uint32_t stride;
  uint32_t count;
};

// Each row is sorted as its varying bytes packed into a key, in significance
// order, with its position as the index and its row index as the value. Ties
// break on position, so the sort is stable.

// Buffers one sort works in.
struct Scratch {
  FlexVector<KeyedIndex> tokens;
  FlexVector<KeyedIndex> spare;
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
      // Row bytes are ordered by significance, even when this word spans
      // multiple columns. Convert to host order so an integer comparison
      // gives the same result as comparing those bytes from left to right.
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
  size_t max_runs = std::min(
      kMaxMergedRuns, static_cast<size_t>(rows.count) / kRowsPerMergedRun);
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
    // XOR each row with the first and accumulate the differences. A byte is
    // constant across all rows exactly when none of its bits are set in diff.
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

// Packs the bytes of `row` at `varying[0, key_bytes)`, most significant
// first, into an integer which orders as they do.
uint64_t PackKey(const uint8_t* row,
                 const uint32_t* varying,
                 uint32_t key_bytes) {
  uint64_t key = 0;
  for (uint32_t b = 0; b < key_bytes; ++b) {
    key = (key << 8) | row[varying[b]];
  }
  return key;
}

// Merges pairs of runs, earlier run first so ties keep their order, until one
// is left. Returns whichever of `tokens` and `scratch` holds the result.
KeyedIndex* MergeRuns(KeyedIndex* tokens,
                      KeyedIndex* scratch,
                      base::SmallVector<uint32_t, kMaxMergedRuns + 2> runs) {
  auto by_key = [](const KeyedIndex& a, const KeyedIndex& b) {
    return a.key < b.key;
  };
  KeyedIndex* source = tokens;
  KeyedIndex* dest = scratch;
  while (runs.size() > 2) {
    size_t kept = 0;
    size_t r = 0;
    for (; r + 2 < runs.size(); r += 2) {
      std::merge(source + runs[r], source + runs[r + 1], source + runs[r + 1],
                 source + runs[r + 2], dest + runs[r], by_key);
      runs[kept++] = runs[r];
    }
    if (r + 1 < runs.size()) {
      // An odd final run has no partner; carry it into the next merge pass.
      memcpy(dest + runs[r], source + runs[r],
             static_cast<size_t>(runs[r + 1] - runs[r]) * sizeof(KeyedIndex));
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

// Below this size, a group of tied tokens is sorted in place.
constexpr uint32_t kInsertionSortTies = 16;

// Sorts tokens by key, then position.
void InsertionSort(KeyedIndex* tokens, uint32_t count) {
  for (uint32_t i = 1; i < count; ++i) {
    KeyedIndex t = tokens[i];
    uint32_t j = i;
    for (; j > 0 &&
           (tokens[j - 1].key > t.key ||
            (tokens[j - 1].key == t.key && tokens[j - 1].index > t.index));
         --j) {
      tokens[j] = tokens[j - 1];
    }
    tokens[j] = t;
  }
}

// Sorts each run of tokens with equal keys on the varying bytes from `from`
// on, 8 at a time, with the same sort as the first 8.
void SortTies(KeyedIndex* tokens,
              uint32_t count,
              const Rows& rows,
              const std::vector<uint32_t>& varying,
              uint32_t from,
              KeyedIndex* spare) {
  auto key_bytes = static_cast<uint32_t>(
      std::min<size_t>(varying.size() - from, kMaxKeyBytes));
  for (uint32_t i = 0; i < count;) {
    uint32_t j = i + 1;
    while (j < count && tokens[j].key == tokens[i].key) {
      ++j;
    }
    if (j - i > 1) {
      KeyedIndex* tied = tokens + i;
      uint32_t size = j - i;
      for (uint32_t t = 0; t < size; ++t) {
        tied[t].key =
            PackKey(rows[tied[t].index], varying.data() + from, key_bytes);
      }
      if (size < kInsertionSortTies) {
        InsertionSort(tied, size);
      } else {
        KeyedIndex* sorted =
            StableSortKeyedIndices(tied, tied + size, spare + i, key_bytes * 8);
        if (sorted != tied) {
          memcpy(tied, sorted, size * sizeof(KeyedIndex));
        }
      }
      if (varying.size() > from + kMaxKeyBytes) {
        SortTies(tied, size, rows, varying, from + kMaxKeyBytes, spare + i);
      }
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
  PERFETTO_DCHECK(row_layout.size() >= static_cast<size_t>(count) * row_stride);
  Rows rows{row_layout.b, row_stride, count};

  // 1. Reuse existing order where possible. Keep run boundaries for merging
  // if the input consists of a small number of ascending runs.
  ExistingOrder order = FindExistingOrder(rows);
  if (order.sorted) {
    return;
  }
  if (order.strictly_descending) {
    // No two rows are equal, so reversing keeps ties in order.
    std::reverse(indices->b, indices->e);
    return;
  }

  // 2. Build compact keys from the first eight varying bytes. Sorting these
  // tokens avoids repeatedly reading full rows through their offsets.
  Scratch scratch;
  FindVaryingBytes(rows, scratch);
  auto key_bytes = static_cast<uint32_t>(
      std::min<size_t>(scratch.varying.size(), kMaxKeyBytes));
  KeyedIndex* tokens = Resize(scratch.tokens, count);
  for (uint32_t r = 0; r < count; ++r) {
    tokens[r] = {PackKey(rows[r], scratch.varying.data(), key_bytes), r,
                 indices->b[r]};
  }
  KeyedIndex* sorted = tokens;
  // 3. Merge existing runs or sort the tokens, preserving input order for ties.
  if (!order.runs.empty()) {
    sorted = MergeRuns(tokens, Resize(scratch.spare, count), order.runs);
  } else {
    sorted = StableSortKeyedIndices(
        tokens, tokens + count, Resize(scratch.spare, count), key_bytes * 8);
  }
  // 4. If the key omitted varying bytes, equal keys may still represent
  // different rows. Sort each tied group on the bytes after the first 8.
  if (scratch.varying.size() > kMaxKeyBytes) {
    // Whichever buffer the sort did not leave its result in is free.
    KeyedIndex* free_buffer =
        sorted == tokens ? Resize(scratch.spare, count) : tokens;
    SortTies(sorted, count, rows, scratch.varying, kMaxKeyBytes, free_buffer);
  }
  for (uint32_t r = 0; r < count; ++r) {
    indices->b[r] = sorted[r].value;
  }
}

}  // namespace perfetto::trace_processor::core::ops
