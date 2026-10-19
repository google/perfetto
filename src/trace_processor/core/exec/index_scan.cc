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

#include "src/trace_processor/core/exec/index_scan.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using filter::CastFilterValueResult;

bool StoresNoValueForNulls(const dataframe::Column& column) {
  const auto& n = column.null_storage.nullability();
  return n.Is<SparseNull>() || n.Is<SparseNullWithPopcountAlways>() ||
         n.Is<SparseNullWithPopcountUntilFinalization>();
}

const void* StorageOf(const dataframe::Column& column) {
  return std::visit([](auto* p) { return static_cast<const void*>(p); },
                    column.storage.data());
}

}  // namespace

IndexScan::IndexScan(
    std::vector<std::shared_ptr<const dataframe::Column>> columns,
    const dataframe::Index& index,
    std::vector<Equality> equalities,
    const StringPool* pool)
    : columns_(std::move(columns)),
      permutation_(index.permutation_vector()),
      equalities_(std::move(equalities)),
      pool_(pool) {
  PERFETTO_CHECK(equalities_.size() <= index.columns().size());
  for (const Equality& eq : equalities_) {
    PERFETTO_CHECK(!eq.column->storage.type().Is<Id>());
  }
}

IndexScan::~IndexScan() = default;
IndexScan::State::~State() = default;

std::unique_ptr<OperatorState> IndexScan::MakeState(Context& context) const {
  auto state = std::make_unique<State>();
  state->reader.Init(columns_, context);
  state->context = &context;
  state->popcounts.resize(equalities_.size());
  for (uint32_t i = 0; i < equalities_.size(); ++i) {
    const dataframe::Column& column = *equalities_[i].column;
    const BitVector* nulls = column.null_storage.MaybeGetNullBitVector();
    if (StoresNoValueForNulls(column)) {
      state->popcounts[i] = nulls->PrefixPopcount();
    }
    state->columns.push_back(
        {column.storage.type(), column.null_storage.nullability(),
         StorageOf(column), nulls, state->popcounts[i].data(), pool_});
  }
  return state;
}

void IndexScan::Rewind(OperatorState& state) const {
  state.Cast<State>().searched = false;
}

Span<const uint32_t> IndexScan::Find(const State& s) const {
  // The searches only read the index.
  auto* b = const_cast<uint32_t*>(permutation_->data());
  Span<uint32_t> entries(b, b + permutation_->size());
  const FilterValue* values = s.context->filter_values().b;
  for (uint32_t i = 0; i < equalities_.size() && !entries.empty(); ++i) {
    const Equality& eq = equalities_[i];
    PERFETTO_DCHECK(eq.value < s.context->filter_values().size());
    const FilterValue& cast = values[eq.value];
    if (cast.value.validity == CastFilterValueResult::kNoneMatch) {
      entries.e = entries.b;
      break;
    }
    if (cast.value.validity == CastFilterValueResult::kAllMatch) {
      continue;
    }
    const filter::IndexColumn& column = s.columns[i];
    if (column.type.Is<String>()) {
      // String = values are cast to an interned id.
      entries = filter::SearchIndexEqual(
          column,
          CastFilterValueResult::Value(pool_->Get(cast.string_id).c_str()),
          entries);
    } else {
      entries = filter::SearchIndexEqual(column, cast.value.value, entries);
    }
  }
  return Span<const uint32_t>(entries.b, entries.e);
}

bool IndexScan::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  if (PERFETTO_UNLIKELY(!s.searched)) {
    s.reader.Start(Find(s));
    s.searched = true;
  }
  return s.reader.Read(out);
}

}  // namespace perfetto::trace_processor::core::exec
