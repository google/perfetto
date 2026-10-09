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

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "perfetto/ext/base/murmur_hash.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr uint32_t kSaltShift = 48;

uint64_t Salt(uint64_t hash) {
  return hash >> kSaltShift << kSaltShift;
}

// Copies the column's values at the rows kept into `out`, widened to int64.
template <typename T>
void Gather(const ColumnView& column,
            const Selection& selection,
            uint32_t count,
            int64_t* out) {
  const auto* data = static_cast<const T*>(column.data());
  for (uint32_t row = 0; row < count; ++row) {
    out[row] = data[column.Index(selection[row])];
  }
}

}  // namespace

std::optional<GroupTable::Kind> GroupTable::Load(const ColumnView& column,
                                                 const Selection& selection,
                                                 uint32_t k,
                                                 uint32_t count) {
  int64_t* keys = keys_[k].data();
  Kind kind;
  if (column.kind() == ColumnView::Kind::kSequence) {
    kind = Kind::kInteger;
    for (uint32_t row = 0; row < count; ++row) {
      keys[row] = column.Index(selection[row]);
    }
  } else if (column.kind() != ColumnView::Kind::kFlat) {
    return std::nullopt;
  } else if (column.type().Is<Uint32>()) {
    kind = Kind::kInteger;
    Gather<uint32_t>(column, selection, count, keys);
  } else if (column.type().Is<Int32>()) {
    kind = Kind::kInteger;
    Gather<int32_t>(column, selection, count, keys);
  } else if (column.type().Is<Int64>() || column.type().Is<Double>()) {
    // A double is its bits.
    kind = column.type().Is<Int64>() ? Kind::kInteger : Kind::kDouble;
    Gather<int64_t>(column, selection, count, keys);
  } else if (column.type().Is<String>()) {
    // A string is its interned id; a null one is the null id, not a null key.
    kind = Kind::kString;
    Gather<uint32_t>(column, selection, count, keys);
    if (const BitVector* validity = column.validity()) {
      for (uint32_t row = 0; row < count; ++row) {
        keys[row] &= -int64_t{validity->is_set(column.Index(selection[row]))};
      }
    }
    return kind;
  } else {
    return std::nullopt;
  }
  if (const BitVector* validity = column.validity()) {
    for (uint32_t row = 0; row < count; ++row) {
      bool valid = validity->is_set(column.Index(selection[row]));
      keys[row] &= -int64_t{valid};
      nulls_[row] |= uint64_t{!valid} << k;
    }
  }
  return kind;
}

std::optional<uint32_t> GroupTable::FindOrCreate(
    const RowBatch& batch,
    const std::vector<uint32_t>& columns,
    uint32_t* groups) {
  uint32_t count = batch.size();
  keys_.resize(columns.size());
  nulls_.resize(count);
  memset(nulls_.data(), 0, count * sizeof(uint64_t));
  bool first = kinds_.empty();
  bool may_be_null = false;
  for (uint32_t k = 0; k < columns.size(); ++k) {
    keys_[k].resize(count);
    may_be_null |= batch.column(columns[k]).validity() != nullptr;
    std::optional<Kind> kind =
        Load(batch.column(columns[k]), batch.selection(), k, count);
    if (!kind || (!first && kinds_[k] != *kind)) {
      // The first batch's kinds are only kept if every column has one.
      if (first) {
        kinds_.clear();
      }
      return k;
    }
    if (first) {
      kinds_.push_back(*kind);
    }
  }
  stride_ = static_cast<uint32_t>(columns.size()) + 1 + payload_words_;

  Grow(size() + count);
  if (keys_.size() == 1 && !may_be_null) {
    FindValues(count, groups);
    return std::nullopt;
  }
  uint64_t mask = slots_.size() - 1;
  for (uint32_t row = 0; row < count; ++row) {
    uint64_t hash = Hash(row);
    for (uint64_t at = hash & mask;; at = (at + 1) & mask) {
      uint64_t slot = slots_[at];
      if (slot == 0) {
        groups[row] = Add(row, hash);
        slots_[at] = Salt(hash) | (groups[row] + 1);
        break;
      }
      auto group = static_cast<uint32_t>((slot & ~Salt(slot)) - 1);
      if (Salt(slot) == Salt(hash) && Equal(group, row)) {
        groups[row] = group;
        break;
      }
    }
  }
  return std::nullopt;
}

void GroupTable::FindValues(uint32_t count, uint32_t* groups) {
  const int64_t* keys = keys_[0].data();
  uint64_t mask = slots_.size() - 1;
  for (uint32_t row = 0; row < count; ++row) {
    int64_t key = keys[row];
    uint64_t hash = base::MurmurHashCombine(key);
    uint32_t group;
    for (uint64_t at = hash & mask;; at = (at + 1) & mask) {
      uint64_t slot = slots_[at];
      if (slot == 0) {
        group = Add(row, hash);
        slots_[at] = Salt(hash) | (group + 1);
        break;
      }
      group = static_cast<uint32_t>((slot & ~Salt(slot)) - 1);
      // A group's row is its nulls, none here, then its key.
      const int64_t* stored = group_rows_.data() + uint64_t{group} * stride_;
      if (Salt(slot) == Salt(hash) && stored[1] == key && stored[0] == 0) {
        break;
      }
    }
    groups[row] = group;
  }
}

uint32_t GroupTable::Add(uint32_t row, uint64_t hash) {
  uint32_t group = size();
  hashes_.push_back(hash);
  group_rows_.push_back(static_cast<int64_t>(nulls_[row]));
  for (const FlexVector<int64_t>& keys : keys_) {
    group_rows_.push_back(keys[row]);
  }
  group_rows_.push_back_multiple(0, payload_words_);
  return group;
}

uint64_t GroupTable::Hash(uint32_t row) const {
  if (keys_.size() == 1 && !nulls_[row]) {
    return base::MurmurHashCombine(keys_[0][row]);
  }
  base::MurmurHashCombiner hasher;
  for (const FlexVector<int64_t>& keys : keys_) {
    hasher.Combine(keys[row]);
  }
  if (nulls_[row]) {
    hasher.Combine(nulls_[row]);
  }
  return hasher.digest();
}

bool GroupTable::Equal(uint32_t group, uint32_t row) const {
  const int64_t* stored = group_rows_.data() + uint64_t{group} * stride_;
  if (static_cast<uint64_t>(stored[0]) != nulls_[row]) {
    return false;
  }
  for (uint32_t k = 0; k < keys_.size(); ++k) {
    if (stored[k + 1] != keys_[k][row]) {
      return false;
    }
  }
  return true;
}

void GroupTable::Place(uint32_t group) {
  uint64_t mask = slots_.size() - 1;
  uint64_t hash = hashes_[group];
  uint64_t at = hash & mask;
  while (slots_[at] != 0) {
    at = (at + 1) & mask;
  }
  slots_[at] = Salt(hash) | (group + 1);
}

// Keeps at most two groups for every three slots, doubling as needed.
void GroupTable::Grow(uint32_t groups) {
  uint64_t capacity = slots_.empty() ? 2 * kMaxBatchRows : slots_.size();
  while (uint64_t{groups} * 3 > capacity * 2) {
    capacity *= 2;
  }
  if (capacity == slots_.size()) {
    return;
  }
  slots_.clear();
  slots_.push_back_multiple(0, capacity);
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
