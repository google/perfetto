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

#include "src/trace_processor/core/exec/batch_order.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// The ordering columns of a batch laid out row by row, so comparing two rows
// reads two short runs of memory: `values` holds the columns of row r at
// [r * width, (r + 1) * width), and `nulls` the same for which are null when
// any column can be. A value is stored with its sign bit flipped, so that
// comparing as unsigned, byte by byte from the top, orders it as signed.
struct Keys {
  uint32_t width = 0;
  bool nullable = false;
  std::vector<uint64_t> values;
  std::vector<uint8_t> nulls;

  // Whether row `a` sorts before row `b`. A null sorts first.
  bool Less(uint32_t a, uint32_t b) const {
    const uint64_t* va = values.data() + a * width;
    const uint64_t* vb = values.data() + b * width;
    if (nullable) {
      const uint8_t* na = nulls.data() + a * width;
      const uint8_t* nb = nulls.data() + b * width;
      for (uint32_t c = 0; c < width; ++c) {
        if (na[c] != nb[c]) {
          return na[c];
        }
        if (!na[c] && va[c] != vb[c]) {
          return va[c] < vb[c];
        }
      }
      return false;
    }
    for (uint32_t c = 0; c < width; ++c) {
      if (va[c] != vb[c]) {
        return va[c] < vb[c];
      }
    }
    return false;
  }
};

// A row with the value of the column being sorted on: a pass moves these
// whole, so it reads them in order and never gathers.
struct KeyedRow {
  uint64_t key;
  uint32_t row;
  // 0 for a null, which sorts first.
  uint32_t present;
};

class OrderState : public OperatorState {
 public:
  ~OrderState() override;

  Keys keys;
  // The two buffers the passes of the sort move the rows between.
  std::vector<KeyedRow> rows;
  std::vector<KeyedRow> scratch;
  // The permutation the batch is composed with, which the batch borrows.
  std::vector<uint32_t> order;
  base::Status status = base::OkStatus();
};

OrderState::~OrderState() = default;

// Lays column `c` of `keys` out from `column`.
void Flatten(const ColumnView& column, uint32_t count, uint32_t c, Keys& keys) {
  const auto* data = static_cast<const int64_t*>(column.data());
  RowSelection selection = column.selection();
  const BitVector* validity = column.validity();
  uint64_t* values = keys.values.data() + c;
  uint32_t width = keys.width;
  if (selection.is_range() && !validity) {
    data += selection.offset();
    for (uint32_t row = 0; row < count; ++row) {
      values[row * width] =
          static_cast<uint64_t>(data[row]) ^ (uint64_t{1} << 63);
    }
    return;
  }
  for (uint32_t row = 0; row < count; ++row) {
    uint32_t index = selection.GetIndex(row);
    values[row * width] =
        static_cast<uint64_t>(data[index]) ^ (uint64_t{1} << 63);
    if (validity) {
      keys.nulls[row * width + c] = !validity->is_set(index);
    }
  }
}

// A pass sorts on this many bits of a key. A byte's worth of buckets keeps
// the destinations a pass scatters into within the cache.
constexpr uint32_t kDigitBits = 8;
constexpr uint64_t kDigitMask = (uint64_t{1} << kDigitBits) - 1;

// One stable counting pass: moves the `count` rows of `from` into `to` by
// `digit` of each, which has `kBuckets` values. `count` is at least one.
//
// Rows with one digit tend to come in runs, as the rows of one thread do
// once ordered by time, and a counter updated once per row would then wait
// on its own previous update. A run is counted in a register instead, and
// its position is carried in one; the counter is touched when a run ends.
template <uint32_t kBuckets, typename Digit>
void CountingPass(const KeyedRow* from,
                  KeyedRow* to,
                  uint32_t count,
                  Digit digit) {
  uint32_t counts[kBuckets] = {};
  uint32_t current = digit(from[0]);
  uint32_t run = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t d = digit(from[i]);
    if (d == current) {
      ++run;
      continue;
    }
    counts[current] += run;
    current = d;
    run = 1;
  }
  counts[current] += run;
  uint32_t total = 0;
  for (uint32_t& bucket : counts) {
    uint32_t size = bucket;
    bucket = total;
    total += size;
  }
  current = digit(from[0]);
  uint32_t position = counts[current];
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t d = digit(from[i]);
    if (d != current) {
      counts[current] = position;
      position = counts[d];
      current = d;
    }
    to[position++] = from[i];
  }
}

}  // namespace

BatchOrder::BatchOrder(std::vector<uint32_t> columns, uint32_t ordered_by_last)
    : columns_(std::move(columns)), ordered_by_last_(ordered_by_last) {
  PERFETTO_CHECK(ordered_by_last_ <= columns_.size());
}
BatchOrder::~BatchOrder() = default;

std::unique_ptr<OperatorState> BatchOrder::MakeState() const {
  return std::make_unique<OrderState>();
}

base::Status BatchOrder::status(const OperatorState& state) const {
  return state.Cast<const OrderState>().status;
}

OpResult BatchOrder::Execute(const RowBatch& in,
                             RowBatch& out,
                             OperatorState& state) const {
  OrderState& s = state.Cast<OrderState>();
  uint32_t count = in.size();
  for (uint32_t column : columns_) {
    const ColumnView& view = in.column(column);
    if (view.kind() != ColumnView::Kind::kFlat || !view.type().Is<Int64>()) {
      s.status = base::ErrStatus("ORDER BY: columns must be Int64");
      return OpResult::kError;
    }
  }
  // The columns the rows already arrive ordered by are never read: rows in
  // order by the ones before them are in order altogether.
  Keys& keys = s.keys;
  keys.width = static_cast<uint32_t>(columns_.size()) - ordered_by_last_;
  keys.nullable = false;
  for (uint32_t c = 0; c < keys.width; ++c) {
    keys.nullable =
        keys.nullable || in.column(columns_[c]).validity() != nullptr;
  }
  out.CopyFrom(in);
  if (keys.width == 0) {
    return OpResult::kNeedMoreInput;
  }
  keys.values.resize(count * keys.width);
  if (keys.nullable) {
    keys.nulls.assign(count * keys.width, 0);
  }
  for (uint32_t c = 0; c < keys.width; ++c) {
    Flatten(in.column(columns_[c]), count, c, keys);
  }
  bool ordered = true;
  for (uint32_t row = 1; ordered && row < count; ++row) {
    ordered = !keys.Less(row, row - 1);
  }
  if (ordered) {
    return OpResult::kNeedMoreInput;
  }

  // Least significant column first, and within a column its bytes from the
  // lowest: each pass is stable, so the order the earlier passes made holds
  // among rows the later ones tie. Only the columns the rows do not already
  // arrive ordered by are sorted. A column is sorted on its distance from its
  // smallest value, which differs from row to row in fewer digits than the
  // values do, and only those digits get a pass.
  KeyedRow* from = s.rows.data();
  KeyedRow* to = s.scratch.data();
  if (s.rows.size() < count) {
    s.rows.resize(count);
    s.scratch.resize(count);
    from = s.rows.data();
    to = s.scratch.data();
  }
  for (uint32_t i = 0; i < count; ++i) {
    from[i].row = i;
  }
  const uint64_t* v = keys.values.data();
  const uint8_t* n = keys.nulls.data();
  uint32_t width = keys.width;
  for (uint32_t c = width; c-- > 0;) {
    uint64_t min = v[c];
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t row = from[i].row;
      from[i].key = v[row * width + c];
      from[i].present = keys.nullable ? !n[row * width + c] : 1;
      min = std::min(min, from[i].key);
    }
    uint64_t varies = 0;
    for (uint32_t i = 0; i < count; ++i) {
      from[i].key -= min;
      varies |= from[i].key;
    }
    for (uint32_t shift = 0; shift < 64; shift += kDigitBits) {
      if (((varies >> shift) & kDigitMask) == 0) {
        continue;
      }
      CountingPass<1u << kDigitBits>(
          from, to, count, [shift](const KeyedRow& r) {
            return static_cast<uint32_t>((r.key >> shift) & kDigitMask);
          });
      std::swap(from, to);
    }
    if (keys.nullable) {
      CountingPass<2>(from, to, count,
                      [](const KeyedRow& r) { return r.present; });
      std::swap(from, to);
    }
  }
  s.order.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    s.order[i] = from[i].row;
  }
  out.Compose(RowSelection::Indices({s.order.data(), s.order.data() + count}),
              count);
  return OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
