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

#include "src/trace_processor/core/exec/eq_scans.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/sort_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/range.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using filter::CastFilterValueResult;

// The run's value `value`, or null if it decides the search alone: no row
// matches it, which empties `range`, or every row does.
PERFETTO_ALWAYS_INLINE const CastFilterValueResult::Value*
ValueToFind(const Context& context, uint32_t value, Range& range) {
  PERFETTO_DCHECK(value < context.filter_values().size());
  const CastFilterValueResult& cast = context.filter_values().b[value].value;
  if (PERFETTO_UNLIKELY(cast.validity != CastFilterValueResult::kValid)) {
    if (cast.validity == CastFilterValueResult::kNoneMatch) {
      range.e = range.b;
    }
    return nullptr;
  }
  return &cast.value;
}

std::unique_ptr<OperatorState> MakeEqScanState(
    const std::vector<std::shared_ptr<const dataframe::Column>>& columns,
    Context& context) {
  auto state = std::make_unique<EqScanState>();
  state->reader.Init(columns, context);
  state->context = &context;
  return state;
}

// Searches on the first batch of a run, then reads what the search found.
template <typename Scan>
PERFETTO_ALWAYS_INLINE bool ReadEqScan(const Scan& scan,
                                       RowBatch& out,
                                       OperatorState& state) {
  auto& s = state.Cast<EqScanState>();
  if (PERFETTO_UNLIKELY(!s.searched)) {
    s.reader.Start(scan.Find(*s.context));
    s.searched = true;
  }
  return s.reader.Read(out);
}

}  // namespace

EqScanState::~EqScanState() = default;

// -----------------------------------------------------------------------------
// IdEqScan
// -----------------------------------------------------------------------------

IdEqScan::IdEqScan(
    std::vector<std::shared_ptr<const dataframe::Column>> columns,
    uint32_t row_count,
    uint32_t value)
    : columns_(std::move(columns)),
      layout_(DataframeLayout(columns_)),
      row_count_(row_count),
      value_(value) {}

IdEqScan::~IdEqScan() = default;

std::unique_ptr<OperatorState> IdEqScan::MakeState(Context& context) const {
  return MakeEqScanState(columns_, context);
}

void IdEqScan::Rewind(OperatorState& state) const {
  state.Cast<EqScanState>().searched = false;
}

bool IdEqScan::GetData(RowBatch& out, OperatorState& state) const {
  return ReadEqScan(*this, out, state);
}

Range IdEqScan::Find(const Context& context) const {
  Range range{0, row_count_};
  if (const auto* v = ValueToFind(context, value_, range)) {
    filter::IdEqualRange(
        base::unchecked_get<CastFilterValueResult::Id>(*v).value, range);
  }
  return range;
}

// -----------------------------------------------------------------------------
// SetIdEqScan
// -----------------------------------------------------------------------------

SetIdEqScan::SetIdEqScan(
    std::vector<std::shared_ptr<const dataframe::Column>> columns,
    uint32_t row_count,
    std::shared_ptr<const dataframe::Column> searched,
    uint32_t value)
    : columns_(std::move(columns)),
      layout_(DataframeLayout(columns_)),
      row_count_(row_count),
      searched_(std::move(searched)),
      value_(value) {
  PERFETTO_CHECK(searched_->storage.type().Is<Uint32>());
  PERFETTO_CHECK(searched_->sort_state.Is<SetIdSorted>());
  PERFETTO_CHECK(searched_->null_storage.nullability().Is<NonNull>());
}

SetIdEqScan::~SetIdEqScan() = default;

std::unique_ptr<OperatorState> SetIdEqScan::MakeState(Context& context) const {
  return MakeEqScanState(columns_, context);
}

void SetIdEqScan::Rewind(OperatorState& state) const {
  state.Cast<EqScanState>().searched = false;
}

bool SetIdEqScan::GetData(RowBatch& out, OperatorState& state) const {
  return ReadEqScan(*this, out, state);
}

Range SetIdEqScan::Find(const Context& context) const {
  Range range{0, row_count_};
  if (const auto* v = ValueToFind(context, value_, range)) {
    filter::SetIdSortedEqualRange(searched_->storage.unchecked_data<Uint32>(),
                                  base::unchecked_get<uint32_t>(*v), range);
  }
  return range;
}

// -----------------------------------------------------------------------------
// SmallValueEqScan
// -----------------------------------------------------------------------------

SmallValueEqScan::SmallValueEqScan(
    std::vector<std::shared_ptr<const dataframe::Column>> columns,
    uint32_t row_count,
    std::shared_ptr<const dataframe::Column> searched,
    uint32_t value)
    : columns_(std::move(columns)),
      layout_(DataframeLayout(columns_)),
      row_count_(row_count),
      searched_(std::move(searched)),
      value_(value) {
  PERFETTO_CHECK(searched_->storage.type().Is<Uint32>());
  PERFETTO_CHECK(searched_->specialized_storage
                     .Is<dataframe::SpecializedStorage::SmallValueEq>());
}

SmallValueEqScan::~SmallValueEqScan() = default;

std::unique_ptr<OperatorState> SmallValueEqScan::MakeState(
    Context& context) const {
  return MakeEqScanState(columns_, context);
}

void SmallValueEqScan::Rewind(OperatorState& state) const {
  state.Cast<EqScanState>().searched = false;
}

bool SmallValueEqScan::GetData(RowBatch& out, OperatorState& state) const {
  return ReadEqScan(*this, out, state);
}

Range SmallValueEqScan::Find(const Context& context) const {
  Range range{0, row_count_};
  if (const auto* v = ValueToFind(context, value_, range)) {
    const auto& small =
        searched_->specialized_storage
            .unchecked_get<dataframe::SpecializedStorage::SmallValueEq>();
    filter::SmallValueEqualRange(small.bit_vector, small.prefix_popcount.data(),
                                 base::unchecked_get<uint32_t>(*v), range);
  }
  return range;
}

}  // namespace perfetto::trace_processor::core::exec
