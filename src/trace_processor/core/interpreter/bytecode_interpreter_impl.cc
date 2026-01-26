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
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/interpreter/bytecode_interpreter_state.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/interpreter/interpreter_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/sort.h"
#include "src/trace_processor/core/util/span.h"
#include "src/trace_processor/util/glob.h"
#include "src/trace_processor/util/regex.h"

namespace perfetto::trace_processor::core::interpreter::ops {

namespace {

struct SortToken {
  uint32_t index;
  uint32_t buf_offset;
};

struct StringSortToken {
  std::string_view str_view;
  StringPool::Id id;
};

// Crossover point where our custom RadixSort starts becoming faster than
// std::stable_sort.
//
// Empirically chosen by looking at the crossover point of benchmarks
// BM_DataframeSortLsdRadix and BM_DataframeSortLsdStd.
constexpr uint32_t kStableSortCutoff = 4096;

struct GlobComparator {
  bool operator()(StringPool::Id lhs, const util::GlobMatcher& m) const {
    return m.Matches(pool->Get(lhs));
  }
  const StringPool* pool;
};

struct BitVectorComparator {
  bool operator()(StringPool::Id lhs, const BitVector& m) const {
    return m.is_set(lhs.raw_id());
  }
};

struct RegexComparator {
  bool operator()(StringPool::Id lhs, const regex::Regex& r) const {
    return r.Search(pool->Get(lhs).c_str());
  }
  const StringPool* pool;
};

}  // namespace

void SortRowLayoutImpl(const Slab<uint8_t>& buffer,
                       uint32_t stride,
                       Span<uint32_t>& indices) {
  auto num_indices = static_cast<size_t>(indices.e - indices.b);

  // Single element is always sorted.
  if (num_indices <= 1) {
    return;
  }

  const uint8_t* buf = buffer.data();

  // Initially do *not* default initialize the array for performance.
  std::unique_ptr<SortToken[]> p(new SortToken[num_indices]);
  std::unique_ptr<SortToken[]> q;
  for (uint32_t i = 0; i < num_indices; ++i) {
    p[i] = {indices.b[i], i * stride};
  }

  SortToken* res;
  if (num_indices < kStableSortCutoff) {
    std::stable_sort(p.get(), p.get() + num_indices,
                     [buf, stride](const SortToken& a, const SortToken& b) {
                       return memcmp(buf + a.buf_offset, buf + b.buf_offset,
                                     stride) < 0;
                     });
    res = p.get();
  } else {
    // We declare q above and populate it here because res might point to q
    // so we need to make sure that q outlives the end of this block.
    // Initially do *not* default initialize the arrays for performance.
    q.reset(new SortToken[num_indices]);
    std::unique_ptr<uint32_t[]> counts(new uint32_t[1 << 16]);
    res = core::RadixSort(
        p.get(), p.get() + num_indices, q.get(), counts.get(), stride,
        [buf](const SortToken& token) { return buf + token.buf_offset; });
  }

  for (uint32_t i = 0; i < num_indices; ++i) {
    indices.b[i] = res[i].index;
  }
}

void FinalizeRanksInMapImpl(
    const StringPool* string_pool,
    std::unique_ptr<base::FlatHashMap<StringPool::Id, uint32_t>>&
        rank_map_ptr) {
  PERFETTO_DCHECK(rank_map_ptr && rank_map_ptr.get());
  auto& rank_map = *rank_map_ptr;

  // Initially do *not* default initialize the array for performance.
  std::unique_ptr<StringSortToken[]> ids_to_sort(
      new StringSortToken[rank_map.size()]);
  std::unique_ptr<StringSortToken[]> scratch(
      new StringSortToken[rank_map.size()]);
  uint32_t i = 0;
  for (auto it = rank_map.GetIterator(); it; ++it) {
    base::StringView str_view = string_pool->Get(it.key());
    ids_to_sort[i++] = StringSortToken{
        std::string_view(str_view.data(), str_view.size()),
        it.key(),
    };
  }
  auto* sorted = core::MsdRadixSort(
      ids_to_sort.get(), ids_to_sort.get() + rank_map.size(), scratch.get(),
      [](const StringSortToken& token) { return token.str_view; });
  for (uint32_t rank = 0; rank < rank_map.size(); ++rank) {
    auto* it = rank_map.Find(sorted[rank].id);
    PERFETTO_DCHECK(it);
    *it = rank;
  }
}

void DistinctImpl(const Slab<uint8_t>& buffer,
                  uint32_t stride,
                  Span<uint32_t>& indices) {
  if (indices.empty()) {
    return;
  }

  const uint8_t* row_ptr = buffer.data();

  std::unordered_set<std::string_view> seen_rows;
  seen_rows.reserve(indices.size());
  uint32_t* write_ptr = indices.b;
  for (const uint32_t* it = indices.b; it != indices.e; ++it) {
    std::string_view row_view(reinterpret_cast<const char*>(row_ptr), stride);
    *write_ptr = *it;
    write_ptr += seen_rows.insert(row_view).second;
    row_ptr += stride;
  }
  indices.e = write_ptr;
}

uint32_t* StringFilterGlobImpl(const StringPool* string_pool,
                               const StringPool::Id* data,
                               const char* pattern,
                               const uint32_t* begin,
                               const uint32_t* end,
                               uint32_t* output) {
  auto matcher = util::GlobMatcher::FromPattern(pattern);

  // If glob pattern doesn't involve any special characters, use equality.
  if (matcher.IsEquality()) {
    std::optional<StringPool::Id> id =
        string_pool->GetId(base::StringView(pattern));
    if (!id) {
      return output;
    }
    const uint32_t* o_read = output;
    uint32_t* o_write = output;
    uint32_t id_raw = id->raw_id();
    for (const uint32_t* it = begin; it != end; ++it, ++o_read) {
      if (data[*it].raw_id() == id_raw) {
        *o_write++ = *o_read;
      }
    }
    return o_write;
  }

  // For very big string pools (or small ranges) or pools with large
  // strings run a standard glob function.
  if (size_t(end - begin) < string_pool->size() ||
      string_pool->HasLargeString()) {
    return ops::Filter(data, begin, end, output, matcher,
                       GlobComparator{string_pool});
  }

  // Pre-compute matches for all strings in the pool.
  auto matches =
      BitVector::CreateWithSize(string_pool->MaxSmallStringId().raw_id());
  PERFETTO_DCHECK(!string_pool->HasLargeString());
  for (auto it = string_pool->CreateSmallStringIterator(); it; ++it) {
    auto id = it.StringId();
    matches.change_assume_unset(id.raw_id(),
                                matcher.Matches(string_pool->Get(id)));
  }

  return ops::Filter(data, begin, end, output, matches, BitVectorComparator{});
}

uint32_t* StringFilterRegexImpl(const StringPool* string_pool,
                                const StringPool::Id* data,
                                const char* pattern,
                                const uint32_t* begin,
                                const uint32_t* end,
                                uint32_t* output) {
  auto regex = regex::Regex::Create(pattern);
  if (!regex.ok()) {
    return output;
  }
  return ops::Filter(data, begin, end, output, regex.value(),
                     RegexComparator{string_pool});
}

void MakeParentToChildTreeStructure(
    InterpreterState& state,
    const struct MakeParentToChildTreeStructure& bytecode) {
  using B = struct MakeParentToChildTreeStructure;

  const TreeStructure::ChildToParent& c2p =
      state.ReadFromRegister(bytecode.arg<B::source_register>());
  TreeStructure::ParentToChild& p2c =
      state.ReadFromRegister(bytecode.arg<B::update_register>());

  auto size = static_cast<uint32_t>(c2p.parents.size());
  if (size == 0) {
    p2c.roots.e = p2c.roots.b;
    p2c.offsets.e = p2c.offsets.b;
    p2c.children.e = p2c.children.b;
    return;
  }

  // First pass: count children for each node and collect roots.
  uint32_t* counts = p2c.offsets.b;
  memset(counts, 0, size * sizeof(uint32_t));
  p2c.roots.e = p2c.roots.b;
  for (uint32_t i = 0; i < size; ++i) {
    uint32_t parent = c2p.parents.b[i];
    if (parent == kTreeNoParent) {
      *p2c.roots.e++ = i;
    } else {
      ++counts[parent];
    }
  }

  // Second pass: convert counts to offsets (exclusive prefix sum) in-place.
  uint32_t running_sum = 0;
  for (uint32_t i = 0; i < size; ++i) {
    uint32_t count = counts[i];
    counts[i] = running_sum;
    running_sum += count;
  }
  counts[size] = running_sum;
  p2c.offsets.e = counts + size + 1;

  // Third pass: populate children array using offsets as write cursors.
  uint32_t* offsets = p2c.offsets.b;
  p2c.children.e = p2c.children.b + running_sum;
  for (uint32_t i = 0; i < size; ++i) {
    uint32_t parent = c2p.parents.b[i];
    if (parent != kTreeNoParent) {
      p2c.children.b[offsets[parent]++] = i;
    }
  }

  // Fourth pass: restore offsets (third pass left offsets[i] = original[i+1]).
  for (uint32_t i = size; i > 0; --i) {
    offsets[i] = offsets[i - 1];
  }
  offsets[0] = 0;
}

void FilterTree(InterpreterState& state, const struct FilterTree& bytecode) {
  using B = struct FilterTree;
  const auto& p2c = state.ReadFromRegister(bytecode.arg<B::source_register>());
  const BitVector* filter_bv =
      state.ReadFromRegister(bytecode.arg<B::filter_register>());
  auto& c2p = state.ReadFromRegister(bytecode.arg<B::update_register>());

  uint32_t size = static_cast<uint32_t>(c2p.parents.size());
  if (size == 0 || !filter_bv) {
    return;
  }

  uint32_t* parents = c2p.parents.b;
  uint32_t* orig_rows = c2p.original_rows.b;

  constexpr uint32_t kNoParent = kTreeNoParent;

  // DFS: for each node, pass down the nearest surviving ancestor's new_idx.
  // When a node survives, it becomes the new ancestor for its children.
  struct Entry {
    uint32_t idx;
    uint32_t ancestor;
  };
  FlexVector<Entry> stack;
  FlexVector<Entry> survivors;

  // Push roots in reverse order for correct traversal order.
  for (auto ri = static_cast<uint32_t>(p2c.roots.size()); ri > 0; --ri) {
    stack.push_back({p2c.roots.b[ri - 1], kNoParent});
  }

  while (!stack.empty()) {
    Entry entry = stack.back();
    stack.pop_back();

    uint32_t new_ancestor = entry.ancestor;
    if (filter_bv->is_set(entry.idx)) {
      new_ancestor = static_cast<uint32_t>(survivors.size());
      survivors.push_back({entry.idx, entry.ancestor});
    }

    // Push children in reverse order.
    uint32_t cs = p2c.offsets.b[entry.idx];
    uint32_t ce = p2c.offsets.b[entry.idx + 1];
    for (uint32_t ci = ce; ci > cs; --ci) {
      stack.push_back({p2c.children.b[ci - 1], new_ancestor});
    }
  }

  // Write output in-place.
  uint32_t num_survivors = static_cast<uint32_t>(survivors.size());

  // Save orig_rows values before overwriting.
  for (uint32_t i = 0; i < num_survivors; ++i) {
    survivors[i].idx = orig_rows[survivors[i].idx];
  }

  // Write output arrays.
  for (uint32_t i = 0; i < num_survivors; ++i) {
    parents[i] = survivors[i].ancestor;
    orig_rows[i] = survivors[i].idx;
  }

  c2p.parents.e = parents + num_survivors;
  c2p.original_rows.e = orig_rows + num_survivors;
}

}  // namespace perfetto::trace_processor::core::interpreter::ops
