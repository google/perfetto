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

#include "src/trace_processor/core/exec/group_table.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>

#include "perfetto/base/bits.h"
#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr uint32_t kSaltShift = 48;
constexpr uint64_t kGroupMask = (uint64_t{1} << kSaltShift) - 1;
constexpr uint64_t kSaltMask = ~kGroupMask;

uint64_t Salt(uint64_t hash) {
  return hash & kSaltMask;
}

uint32_t GroupFromSlot(uint64_t slot) {
  return static_cast<uint32_t>(slot & kGroupMask) - 1;
}

// Calls `fn(row)` for each row in [0, count) that is null in `validity`,
// reading the bits from `start` 64 at a time.
template <typename Fn>
PERFETTO_ALWAYS_INLINE inline void ForEachNull(const BitVector& validity,
                                               uint32_t start,
                                               uint32_t count,
                                               Fn fn) {
  for (uint32_t row = 0; row < count; row += 64) {
    uint32_t n = std::min(64u, count - row);
    uint64_t mask = n == 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1;
    uint64_t nulls = ~validity.ReadBits(start + row, n) & mask;
    for (; nulls; nulls &= nulls - 1) {
      fn(row + base::CountTrailZeros64(nulls));
    }
  }
}

// Copies the column's values for the selected rows into `out` as int64.
template <typename T>
void Gather(const ColumnView& column,
            const Selection& selection,
            uint32_t count,
            int64_t* out) {
  const auto* data = static_cast<const T*>(column.data()) + column.Index(0);
  if (selection.prefix()) {
    if constexpr (std::is_same_v<T, int64_t>) {
      memcpy(out, data, count * sizeof(int64_t));
    } else {
      for (uint32_t row = 0; row < count; ++row) {
        out[row] = data[row];
      }
    }
    return;
  }
  // A gather, which vector instructions do not speed up.
  PERFETTO_NO_VECTORIZE_LOOP
  for (uint32_t row = 0; row < count; ++row) {
    out[row] = data[selection[row]];
  }
}

}  // namespace

std::optional<GroupTable::Kind> GroupTable::Load(const ColumnView& column,
                                                 const Selection& selection,
                                                 uint32_t k,
                                                 uint32_t count) {
  int64_t* keys = this->keys(k);
  Kind kind;
  if (column.kind() == ColumnView::Kind::kSequence) {
    kind = Kind::kInteger;
    if (selection.prefix()) {
      uint32_t start = column.start();
      for (uint32_t row = 0; row < count; ++row) {
        keys[row] = start + row;
      }
    } else {
      for (uint32_t row = 0; row < count; ++row) {
        keys[row] = column.Index(selection[row]);
      }
    }
  } else if (column.kind() != ColumnView::Kind::kFlat) {
    return std::nullopt;
  } else if (column.type().Is<Uint32>()) {
    kind = Kind::kInteger;
    Gather<uint32_t>(column, selection, count, keys);
  } else if (column.type().Is<Int32>()) {
    kind = Kind::kInteger;
    Gather<int32_t>(column, selection, count, keys);
  } else if (column.type().Is<Int64>()) {
    kind = Kind::kInteger;
    Gather<int64_t>(column, selection, count, keys);
  } else if (column.type().Is<Double>()) {
    // A double is compared by its bits, made the same for equal values such
    // as 0.0 and -0.0.
    kind = Kind::kDouble;
    const auto* data = static_cast<const double*>(column.data());
    if (selection.prefix()) {
      const auto* src = data + column.start();
      for (uint32_t row = 0; row < count; ++row) {
        keys[row] = static_cast<int64_t>(base::CanonicalFloatBits(src[row]));
      }
    } else {
      for (uint32_t row = 0; row < count; ++row) {
        keys[row] = static_cast<int64_t>(
            base::CanonicalFloatBits(data[column.Index(selection[row])]));
      }
    }
  } else if (column.type().Is<String>()) {
    // A string is compared by its interned id. A null string has the null id
    // and is not a null key.
    kind = Kind::kString;
    Gather<uint32_t>(column, selection, count, keys);
    if (const BitVector* validity = column.validity()) {
      if (selection.prefix()) {
        ForEachNull(*validity, column.start(), count,
                    [keys](uint32_t row) { keys[row] = 0; });
      } else {
        for (uint32_t row = 0; row < count; ++row) {
          bool valid = validity->is_set(column.Index(selection[row]));
          keys[row] = valid ? keys[row] : 0;
        }
      }
    }
    return kind;
  } else {
    return std::nullopt;
  }
  if (const BitVector* validity = column.validity()) {
    if (selection.prefix()) {
      uint64_t null_bit = uint64_t{1} << k;
      uint64_t* nulls = nulls_.data();
      ForEachNull(*validity, column.start(), count, [&](uint32_t row) {
        keys[row] = 0;
        nulls[row] |= null_bit;
      });
    } else {
      for (uint32_t row = 0; row < count; ++row) {
        bool valid = validity->is_set(column.Index(selection[row]));
        keys[row] = valid ? keys[row] : 0;
        nulls_[row] |= uint64_t{!valid} << k;
      }
    }
  }
  return kind;
}

std::optional<uint32_t> GroupTable::FindOrCreate(
    const RowBatch& batch,
    base::Span<const uint32_t> columns,
    uint32_t* groups,
    OperatorState& owner) {
  return FindOrCreate(batch, batch.selection(), columns, groups, owner);
}

std::optional<uint32_t> GroupTable::FindOrCreate(
    const RowBatch& batch,
    const Selection& rows,
    base::Span<const uint32_t> columns,
    uint32_t* groups,
    OperatorState& owner) {
  // Nulls are stored as one bit per column in a 64-bit word.
  PERFETTO_CHECK(columns.size() <= 64);
  PERFETTO_DCHECK(kinds_.empty() || kinds_.size() == columns.size());
  uint32_t count = rows.size();
  key_count_ = static_cast<uint32_t>(columns.size());
  keys_.resize(owner, uint64_t{key_count_} * kMaxBatchRows);
  nulls_.resize(owner, count);
  memset(nulls_.data(), 0, count * sizeof(uint64_t));
  bool first = kinds_.empty();
  bool may_be_null = false;
  for (uint32_t k = 0; k < columns.size(); ++k) {
    may_be_null |= batch.column(columns[k]).validity() != nullptr;
    std::optional<Kind> kind = Load(batch.column(columns[k]), rows, k, count);
    if (!kind || (!first && kinds_[k] != *kind)) {
      // Only keep the first batch's kinds if every column is a valid key.
      if (first) {
        kinds_.clear();
      }
      return k;
    }
    if (first) {
      kinds_.push_back(owner, *kind);
    }
  }
  stride_ = key_count_ + 1 + payload_words_;

  Grow(size() + count, owner);
  // Reserve room for every row to add a group, so adding one needs no capacity
  // check.
  hashes_.reserve(owner, uint64_t{size()} + count);
  group_rows_.reserve(owner, (uint64_t{size()} + count) * stride_);
  if (key_count_ == 1 && !may_be_null) {
    FindValues(count, groups);
    return std::nullopt;
  }
  uint64_t mask = slots_.size() - 1;
  uint64_t* slots = slots_.data();
  const int64_t* stored_base = group_rows_.data();
  for (uint32_t row = 0; row < count; ++row) {
    uint64_t hash = Hash(row);
    uint64_t salt = Salt(hash);
    for (uint64_t at = hash & mask;; at = (at + 1) & mask) {
      uint64_t slot = slots[at];
      if (slot == 0) {
        uint32_t group = Add(row, hash);
        slots[at] = salt | (group + 1);
        groups[row] = group;
        break;
      }
      if (Salt(slot) == salt) {
        uint32_t group = GroupFromSlot(slot);
        if (Equal(stored_base, group, row)) {
          groups[row] = group;
          break;
        }
      }
    }
  }
  return std::nullopt;
}

PERFETTO_NO_INLINE void GroupTable::FindValues(uint32_t count,
                                               uint32_t* groups) {
  // Everything is kept in locals, since a store through `groups` could
  // otherwise alias the members. Groups are added into the room FindOrCreate()
  // reserved, and the table's sizes are set once the batch is done.
  const int64_t* keys = this->keys(0);
  uint64_t mask = slots_.size() - 1;
  uint64_t* slots = slots_.data();
  uint64_t stride = stride_;
  uint64_t payload_words = payload_words_;
  uint32_t first = size();
  uint32_t next = first;
  uint64_t* hashes = hashes_.data();
  int64_t* stored_base = group_rows_.data();
  for (uint32_t row = 0; row < count; ++row) {
    int64_t key = keys[row];
    uint64_t hash = base::MurmurHashValue(key);
    uint64_t salt = Salt(hash);
    uint32_t group;
    for (uint64_t at = hash & mask;; at = (at + 1) & mask) {
      uint64_t slot = slots[at];
      if (slot == 0) {
        group = next++;
        hashes[group] = hash;
        int64_t* stored = stored_base + group * stride;
        // A single key with no nulls.
        stored[0] = 0;
        stored[1] = key;
        std::fill_n(stored + 2, payload_words, 0);
        slots[at] = salt | (group + 1);
        break;
      }
      if (Salt(slot) == salt) {
        uint32_t candidate = GroupFromSlot(slot);
        const int64_t* stored = stored_base + candidate * stride;
        // A null group's salt never matches key 0's, the only key it stores.
        if (stored[1] == key) {
          group = candidate;
          break;
        }
      }
    }
    groups[row] = group;
  }
  hashes_.append_reserved(next - first);
  group_rows_.append_reserved(uint64_t{next - first} * stride);
}

// Out of line, so the probe loops keep their values in registers.
PERFETTO_NO_INLINE uint32_t GroupTable::Add(uint32_t row, uint64_t hash) {
  uint32_t group = size();
  *hashes_.append_reserved(1) = hash;
  int64_t* stored = group_rows_.append_reserved(stride_);
  stored[0] = static_cast<int64_t>(nulls_[row]);
  // With only one or two keys per group, vector code would only add checks.
  PERFETTO_NO_VECTORIZE_LOOP
  for (uint32_t k = 0; k < key_count_; ++k) {
    stored[k + 1] = keys(k)[row];
  }
  std::fill_n(stored + key_count_ + 1, payload_words_, 0);
  return group;
}

PERFETTO_ALWAYS_INLINE uint64_t GroupTable::Hash(uint32_t row) const {
  if (key_count_ == 1 && !nulls_[row]) {
    return base::MurmurHashValue(keys(0)[row]);
  }
  if (key_count_ == 2 && !nulls_[row]) {
    return base::MurmurHashCombine(keys(0)[row], keys(1)[row]);
  }
  base::MurmurHashCombiner hasher;
  for (uint32_t k = 0; k < key_count_; ++k) {
    hasher.Combine(keys(k)[row]);
  }
  if (nulls_[row]) {
    hasher.Combine(nulls_[row]);
  }
  return hasher.digest();
}

PERFETTO_ALWAYS_INLINE bool GroupTable::Equal(const int64_t* stored_base,
                                              uint32_t group,
                                              uint32_t row) const {
  const int64_t* stored = stored_base + uint64_t{group} * stride_;
  if (static_cast<uint64_t>(stored[0]) != nulls_[row]) {
    return false;
  }
  switch (key_count_) {
    case 1:
      return stored[1] == keys(0)[row];
    case 2:
      return stored[1] == keys(0)[row] && stored[2] == keys(1)[row];
    default:
      for (uint32_t k = 0; k < key_count_; ++k) {
        if (stored[k + 1] != keys(k)[row]) {
          return false;
        }
      }
      return true;
  }
}

void GroupTable::Place(uint32_t group) {
  uint64_t mask = slots_.size() - 1;
  uint64_t hash = hashes_[group];
  uint64_t salt = Salt(hash);
  uint64_t at = hash & mask;
  while (slots_[at] != 0) {
    at = (at + 1) & mask;
  }
  slots_[at] = salt | (group + 1);
}

// Doubles the slot count until at most two thirds of the slots are used.
void GroupTable::Grow(uint32_t groups, OperatorState& owner) {
  uint64_t capacity = slots_.empty() ? 2 * kMaxBatchRows : slots_.size();
  while (uint64_t{groups} * 3 > capacity * 2) {
    capacity *= 2;
  }
  if (capacity == slots_.size()) {
    return;
  }
  slots_.assign(owner, capacity, 0);
  for (uint32_t group = 0; group < size(); ++group) {
    Place(group);
  }
}

void GroupTable::Clear() {
  slots_.clear();
  hashes_.clear();
  group_rows_.clear();
  stride_ = 0;
  kinds_.clear();
}

}  // namespace perfetto::trace_processor::core::exec
