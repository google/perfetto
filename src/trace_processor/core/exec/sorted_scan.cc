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

#include "src/trace_processor/core/exec/sorted_scan.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
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

// Null for an Id column.
const void* StorageOf(const dataframe::Column& column) {
  return std::visit([](auto* p) { return static_cast<const void*>(p); },
                    column.storage.data());
}

}  // namespace

SortedScan::SortedScan(
    std::vector<std::shared_ptr<const dataframe::Column>> columns,
    uint32_t row_count,
    std::vector<Search> searches,
    const StringPool* pool)
    : columns_(std::move(columns)),
      row_count_(row_count),
      searches_(std::move(searches)),
      pool_(pool) {
  for (const Search& s : searches_) {
    std::optional<RangeOp> op = s.op.TryDowncast<RangeOp>();
    PERFETTO_CHECK(op);
    PERFETTO_CHECK(s.column->null_storage.nullability().Is<NonNull>());
    PERFETTO_CHECK(s.kind == Search::Kind::kBinarySearch || s.op.Is<Eq>());
    StorageType type = s.column->storage.type();
    sorted_.push_back({{type, StorageOf(*s.column), pool_},
                       *op,
                       type.Is<String>() && op->Is<Eq>()});
  }
}

SortedScan::~SortedScan() = default;
SortedScan::State::~State() = default;

std::unique_ptr<OperatorState> SortedScan::MakeState(Context& context) const {
  auto state = std::make_unique<State>();
  state->reader.Init(columns_, context);
  state->context = &context;
  return state;
}

void SortedScan::Rewind(OperatorState& state) const {
  state.Cast<State>().searched = false;
}

Range SortedScan::Find(const Context& context) const {
  const FilterValue* values = context.filter_values().b;
  Range range{0, row_count_};
  for (uint32_t i = 0; i < searches_.size() && !range.empty(); ++i) {
    const Search& s = searches_[i];
    PERFETTO_DCHECK(s.value < context.filter_values().size());
    const FilterValue& cast = values[s.value];
    if (cast.value.validity == CastFilterValueResult::kNoneMatch) {
      range.e = range.b;
      break;
    }
    if (cast.value.validity == CastFilterValueResult::kAllMatch) {
      continue;
    }
    const dataframe::Column& column = *s.column;
    switch (s.kind) {
      case Search::Kind::kBinarySearch: {
        const Sorted& sorted = sorted_[i];
        if (sorted.string_eq) {
          // String = values are cast to an interned id.
          range = filter::SearchSorted(
              sorted.column, sorted.op,
              CastFilterValueResult::Value(pool_->Get(cast.string_id).c_str()),
              range);
        } else {
          range = filter::SearchSorted(sorted.column, sorted.op,
                                       cast.value.value, range);
        }
        break;
      }
      case Search::Kind::kSetIdEq:
        filter::SetIdSortedEqualRange(
            column.storage.unchecked_data<Uint32>(),
            base::unchecked_get<uint32_t>(cast.value.value), range);
        break;
      case Search::Kind::kSmallValueEq: {
        const auto& small =
            column.specialized_storage
                .unchecked_get<dataframe::SpecializedStorage::SmallValueEq>();
        filter::SmallValueEqualRange(
            small.bit_vector, small.prefix_popcount.data(),
            base::unchecked_get<uint32_t>(cast.value.value), range);
        break;
      }
    }
  }
  return range;
}

bool SortedScan::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  if (PERFETTO_UNLIKELY(!s.searched)) {
    s.reader.Start(Find(*s.context));
    s.searched = true;
  }
  return s.reader.Read(out);
}

}  // namespace perfetto::trace_processor::core::exec
