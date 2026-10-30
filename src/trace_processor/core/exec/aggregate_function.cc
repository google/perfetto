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

#include "src/trace_processor/core/exec/aggregate_function.h"

#include <algorithm>
#include <cstdint>
#include <memory>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"

namespace perfetto::trace_processor::core::exec {

void AggregateInputLoader::Load(const ColumnView& column,
                                const Selection& selection,
                                const uint32_t* rows,
                                uint32_t count,
                                AggregateInput* out) {
  PERFETTO_DCHECK(IsFlat<Int64>(column));
  const auto* data = static_cast<const int64_t*>(column.data());
  // Where the `i`th row loaded is stored.
  auto index = [&](uint32_t i) {
    return column.Index(selection[rows ? rows[i] : i]);
  };
  const BitVector* validity = column.validity();
  out->rows = count;
  out->valid = nullptr;
  if (validity) {
    valid_.resize((count + 63) / 64);
    out->valid = valid_.data();
  }
  if (!rows && selection.prefix()) {
    // The rows are a run: read in place, with their validity a word at a time.
    out->values = data + column.start();
    for (uint32_t row = 0; validity && row < count; row += 64) {
      uint32_t n = std::min(64u, count - row);
      valid_[row / 64] = validity->ReadBits(column.Index(row), n);
    }
    return;
  }
  // Gathered, a word of rows at a time: each row's value and validity are read
  // from where it is stored, found once. Vector instructions do not speed up
  // gathers, and only add code.
  values_.resize(count);
  for (uint32_t row = 0; row < count; row += 64) {
    uint32_t n = std::min(64u, count - row);
    uint64_t word = 0;
    PERFETTO_NO_VECTORIZE_LOOP
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t at = index(row + i);
      values_[row + i] = data[at];
      word |= uint64_t{validity && validity->is_set(at)} << i;
    }
    if (validity) {
      valid_[row / 64] = word;
    }
  }
  out->values = values_.data();
}

uint8_t* SeenBytes::For(uint32_t groups, bool nulls) {
  if (!tracking_ && !nulls) {
    groups_ = groups;
    return nullptr;
  }
  if (!tracking_) {
    // Every group so far added only values.
    tracking_ = true;
    bytes_.push_back_multiple(1, groups_);
  }
  bytes_.push_back_multiple(0, groups - bytes_.size());
  groups_ = groups;
  return bytes_.data();
}

void SeenBytes::Clear() {
  bytes_.clear();
  tracking_ = false;
  groups_ = 0;
}

AggregateFunction::~AggregateFunction() = default;

void AggregateFunction::Subtract(const GroupMerge*,
                                 uint32_t,
                                 GroupStates,
                                 bool*) const {
  PERFETTO_FATAL("Subtract on a function which can't");
}

void AggregateFunction::Prefix(uint32_t, GroupStates, bool*) const {
  PERFETTO_FATAL("Prefix on a function which can't subtract");
}

namespace {

// How many rows of `in` hold a value.
uint32_t CountValid(const AggregateInput& in) {
  if (!in.valid) {
    return in.rows;
  }
  uint32_t count = 0;
  for (uint32_t row = 0; row < in.rows; row += 64) {
    uint32_t n = std::min(64u, in.rows - row);
    uint64_t mask = n == 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1;
    count +=
        static_cast<uint32_t>(PERFETTO_POPCOUNT(in.valid[row / 64] & mask));
  }
  return count;
}

// COUNT(*): how many rows a group holds.
class CountStar : public AggregateFunction {
 public:
  uint32_t state_words() const override { return 1; }
  bool reads_input() const override { return false; }
  bool tracks_seen() const override { return false; }
  void Update(const AggregateInput& in,
              const uint32_t* groups,
              GroupStates states,
              bool*) const override {
    for (uint32_t row = 0; row < in.rows; ++row) {
      ++states.words[groups[row] * states.stride];
    }
  }
  void UpdateGroup(const AggregateInput& in,
                   uint32_t group,
                   GroupStates states,
                   bool*) const override {
    states.words[group * states.stride] += in.rows;
  }
  void Combine(const GroupMerge* merges,
               uint32_t count,
               GroupStates states,
               bool*) const override {
    for (uint32_t i = 0; i < count; ++i) {
      states.words[merges[i].into * states.stride] +=
          states.words[merges[i].from * states.stride];
    }
  }
  bool can_subtract() const override { return true; }
  void Subtract(const GroupMerge* merges,
                uint32_t count,
                GroupStates states,
                bool*) const override {
    for (uint32_t i = 0; i < count; ++i) {
      states.words[merges[i].into * states.stride] -=
          states.words[merges[i].from * states.stride];
    }
  }
  void Prefix(uint32_t count, GroupStates states, bool*) const override {
    // The total stays in a register rather than waiting on each store.
    int64_t total = 0;
    for (uint32_t g = 0; g < count; ++g) {
      total += states.words[g * states.stride];
      states.words[g * states.stride] = total;
    }
  }
  void Finalize(GroupStates states,
                const uint32_t* groups,
                uint32_t count,
                int64_t* values,
                BitVector* valid) const override {
    for (uint32_t i = 0; i < count; ++i) {
      values[i] = states.words[groups[i] * states.stride];
    }
    valid->FillBits(0, count, true);
  }
};

// COUNT(column): how many of a group's rows hold a value. Merges and reads
// as COUNT(*) does.
class Count final : public CountStar {
 public:
  bool reads_input() const override { return true; }
  void Update(const AggregateInput& in,
              const uint32_t* groups,
              GroupStates states,
              bool*) const override {
    for (uint32_t row = 0; row < in.rows; ++row) {
      bool valid = !in.valid || ((in.valid[row / 64] >> (row % 64)) & 1);
      states.words[groups[row] * states.stride] += valid;
    }
  }
  void UpdateGroup(const AggregateInput& in,
                   uint32_t group,
                   GroupStates states,
                   bool*) const override {
    states.words[group * states.stride] += CountValid(in);
  }
};

// SUM: the total of a group's values, null if it holds none. State: the
// total.
class Sum final : public AggregateFunction {
 public:
  uint32_t state_words() const override { return 1; }
  bool reads_input() const override { return true; }
  bool tracks_seen() const override { return true; }
  void Update(const AggregateInput& in,
              const uint32_t* groups,
              GroupStates states,
              bool* overflow) const override {
    bool ovf = false;
    for (uint32_t row = 0; row < in.rows; ++row) {
      // A null row's value is stored too: masking it out avoids a branch.
      bool valid = !in.valid || ((in.valid[row / 64] >> (row % 64)) & 1);
      uint32_t group = groups[row];
      int64_t& total = states.words[group * states.stride];
      ovf |= !base::CheckedAdd(total, in.values[row] & -int64_t{valid}, &total);
      if (states.seen) {
        states.seen[group] |= valid;
      }
    }
    *overflow |= ovf;
  }
  void UpdateGroup(const AggregateInput& in,
                   uint32_t group,
                   GroupStates states,
                   bool* overflow) const override {
    // The high and low 32 bits of the values are summed apart, which can't
    // overflow for a batch and vectorizes, unlike checking each addition.
    PERFETTO_DCHECK(in.rows <= kMaxBatchRows);
    int64_t high = 0;
    uint64_t low = 0;
    if (!in.valid) {
      for (uint32_t row = 0; row < in.rows; ++row) {
        high += in.values[row] >> 32;
        low += static_cast<uint32_t>(in.values[row]);
      }
    } else {
      // A word of validity at a time, rather than a bit for each row.
      for (uint32_t row = 0; row < in.rows; row += 64) {
        uint32_t n = std::min(64u, in.rows - row);
        uint64_t valid = in.valid[row / 64];
        for (uint32_t i = 0; i < n; ++i) {
          int64_t value =
              in.values[row + i] & -static_cast<int64_t>((valid >> i) & 1);
          high += value >> 32;
          low += static_cast<uint32_t>(value);
        }
      }
    }
    // The sum is high * 2^32 + low, which fits in 64 bits if, once low's
    // carry is in high, high fits in 32.
    high += static_cast<int64_t>(low >> 32);
    bool fits = high >= INT32_MIN && high <= INT32_MAX;
    auto sum = static_cast<int64_t>(static_cast<uint64_t>(high) << 32 |
                                    (low & 0xffffffff));
    int64_t& total = states.words[group * states.stride];
    *overflow |= !fits || !base::CheckedAdd(total, sum, &total);
    if (states.seen) {
      states.seen[group] |= CountValid(in) != 0;
    }
  }
  void Combine(const GroupMerge* merges,
               uint32_t count,
               GroupStates states,
               bool* overflow) const override {
    bool ovf = false;
    for (uint32_t i = 0; i < count; ++i) {
      int64_t& to = states.words[merges[i].into * states.stride];
      int64_t from = states.words[merges[i].from * states.stride];
      ovf |= !base::CheckedAdd(to, from, &to);
      if (states.seen) {
        states.seen[merges[i].into] |= states.seen[merges[i].from];
      }
    }
    *overflow |= ovf;
  }
  bool can_subtract() const override { return true; }
  void Subtract(const GroupMerge* merges,
                uint32_t count,
                GroupStates states,
                bool* overflow) const override {
    bool ovf = false;
    for (uint32_t i = 0; i < count; ++i) {
      int64_t& to = states.words[merges[i].into * states.stride];
      int64_t from = states.words[merges[i].from * states.stride];
      ovf |= !base::CheckedSub(to, from, &to);
    }
    *overflow |= ovf;
  }
  void Prefix(uint32_t count,
              GroupStates states,
              bool* overflow) const override {
    // The total stays in a register rather than waiting on each store.
    bool ovf = false;
    int64_t total = 0;
    for (uint32_t g = 0; g < count; ++g) {
      ovf |= !base::CheckedAdd(total, states.words[g * states.stride], &total);
      states.words[g * states.stride] = total;
    }
    *overflow |= ovf;
  }
  void Finalize(GroupStates states,
                const uint32_t* groups,
                uint32_t count,
                int64_t* values,
                BitVector* valid) const override {
    for (uint32_t i = 0; i < count; ++i) {
      values[i] = states.words[groups[i] * states.stride];
    }
    if (!states.seen) {
      valid->FillBits(0, count, true);
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      valid->change(i, states.seen[groups[i]]);
    }
  }
};

// MIN or MAX: a group's extreme value, null if it holds none. The state is
// the value XORed with a mask chosen so that, as unsigned words, the larger
// state is the value wanted and zero is below every value: a zeroed state
// holds nothing, and states merge by unsigned maximum.
class Extreme final : public AggregateFunction {
 public:
  explicit Extreme(uint64_t mask) : mask_(mask) {}
  uint32_t state_words() const override { return 1; }
  bool reads_input() const override { return true; }
  bool tracks_seen() const override { return true; }
  void Update(const AggregateInput& in,
              const uint32_t* groups,
              GroupStates states,
              bool*) const override {
    for (uint32_t row = 0; row < in.rows; ++row) {
      bool valid = !in.valid || ((in.valid[row / 64] >> (row % 64)) & 1);
      uint32_t group = groups[row];
      auto& state =
          reinterpret_cast<uint64_t&>(states.words[group * states.stride]);
      uint64_t value =
          (static_cast<uint64_t>(in.values[row]) ^ mask_) & -uint64_t{valid};
      state = std::max(state, value);
      if (states.seen) {
        states.seen[group] |= valid;
      }
    }
  }
  void UpdateGroup(const AggregateInput& in,
                   uint32_t group,
                   GroupStates states,
                   bool*) const override {
    auto& state =
        reinterpret_cast<uint64_t&>(states.words[group * states.stride]);
    uint64_t best = state;
    for (uint32_t row = 0; row < in.rows; ++row) {
      bool valid = !in.valid || ((in.valid[row / 64] >> (row % 64)) & 1);
      uint64_t value =
          (static_cast<uint64_t>(in.values[row]) ^ mask_) & -uint64_t{valid};
      best = std::max(best, value);
    }
    state = best;
    if (states.seen) {
      states.seen[group] |= CountValid(in) != 0;
    }
  }
  void Combine(const GroupMerge* merges,
               uint32_t count,
               GroupStates states,
               bool*) const override {
    for (uint32_t i = 0; i < count; ++i) {
      auto& to = reinterpret_cast<uint64_t&>(
          states.words[merges[i].into * states.stride]);
      to = std::max(to, static_cast<uint64_t>(
                            states.words[merges[i].from * states.stride]));
      if (states.seen) {
        states.seen[merges[i].into] |= states.seen[merges[i].from];
      }
    }
  }
  void Finalize(GroupStates states,
                const uint32_t* groups,
                uint32_t count,
                int64_t* values,
                BitVector* valid) const override {
    for (uint32_t i = 0; i < count; ++i) {
      values[i] = static_cast<int64_t>(
          static_cast<uint64_t>(states.words[groups[i] * states.stride]) ^
          mask_);
    }
    if (!states.seen) {
      valid->FillBits(0, count, true);
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      valid->change(i, states.seen[groups[i]]);
    }
  }

 private:
  const uint64_t mask_;
};

}  // namespace

std::unique_ptr<AggregateFunction> MakeAggregateFunction(
    AggregateCall::Function function) {
  switch (function) {
    case AggregateCall::Function::kCountStar:
      return std::make_unique<CountStar>();
    case AggregateCall::Function::kSum:
      return std::make_unique<Sum>();
    case AggregateCall::Function::kCount:
      return std::make_unique<Count>();
    case AggregateCall::Function::kMin:
      // Flips every bit but the sign: the smallest value is the largest word.
      return std::make_unique<Extreme>(~(uint64_t{1} << 63));
    case AggregateCall::Function::kMax:
      // Flips the sign: words order as the values do, the least being zero.
      return std::make_unique<Extreme>(uint64_t{1} << 63);
  }
  return nullptr;
}

ColumnBuffer FinalizeToBuffer(Context& context,
                              const AggregateFunction& function,
                              GroupStates states,
                              const uint32_t* groups,
                              uint32_t count) {
  ColumnBuffer buffer = context.TakeBuffer(sizeof(int64_t));
  ColumnChunk& chunk = buffer.chunk();
  chunk.SizeValidity();
  function.Finalize(states, groups, count, chunk.Values<int64_t>(),
                    &chunk.validity);
  return buffer;
}

ColumnView ResultView(const ColumnBuffer& buffer) {
  ColumnChunk& chunk = buffer.chunk();
  return ColumnView::Reference(StorageType{Int64{}}, chunk.Values<int64_t>(),
                               &chunk.validity);
}

}  // namespace perfetto::trace_processor::core::exec
