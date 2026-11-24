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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/param_value.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/range.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using filter::CastFilterValueResult;

bool StoresNoValueForNulls(const dataframe::Column& column) {
  const auto& n = column.null_storage.nullability();
  return n.Is<SparseNull>() || n.Is<SparseNullWithPopcountAlways>() ||
         n.Is<SparseNullWithPopcountUntilFinalization>();
}

}  // namespace

IndexScan::IndexScan(
    std::vector<std::shared_ptr<const dataframe::Column>> columns,
    const dataframe::Index& index,
    std::vector<Equality> equalities,
    std::vector<SortedSearches::Search> searches,
    const StringPool* pool)
    : columns_(std::move(columns)),
      permutation_(index.permutation_vector()),
      equalities_(std::move(equalities)),
      searches_(std::move(searches), pool),
      pool_(pool),
      whole_key_(equalities_.size() == index.columns().size()) {
  PERFETTO_CHECK(equalities_.size() <= index.columns().size());
  for (const Equality& eq : equalities_) {
    PERFETTO_CHECK(!eq.column->storage.type().Is<Id>());
  }
}

IndexScan::~IndexScan() = default;
IndexScan::State::~State() = default;

std::unique_ptr<OperatorState> IndexScan::MakeState(Context& context) const {
  auto state = std::make_unique<State>();
  state->reader.Init(columns_, &context);
  state->context = &context;
  state->popcounts.resize(equalities_.size());
  for (uint32_t i = 0; i < equalities_.size(); ++i) {
    const dataframe::Column& column = *equalities_[i].column;
    const BitVector* nulls = column.null_storage.MaybeGetNullBitVector();
    // Built here rather than lazily in Find(), which runs far more often.
    if (StoresNoValueForNulls(column)) {
      state->popcounts[i] = nulls->PrefixPopcount();
    }
    state->columns.push_back({column.storage.type(),
                              column.null_storage.nullability(),
                              column.storage.untyped_data(), nulls,
                              state->popcounts[i].data(), pool_});
  }
  return state;
}

void IndexScan::Rewind(OperatorState& state) const {
  state.Cast<State>().searched = false;
}

base::Span<const uint32_t> IndexScan::Find(State& s) const {
  base::Span<const uint32_t> entries(
      permutation_->data(), permutation_->data() + permutation_->size());
  const ParamValue* values = s.context->params().b;
  bool one_key = whole_key_;
  for (uint32_t i = 0; i < equalities_.size() && !entries.empty(); ++i) {
    const Equality& eq = equalities_[i];
    PERFETTO_DCHECK(eq.param < s.context->params().size());
    const ParamValue& cast = values[eq.param];
    if (cast.value.validity == CastFilterValueResult::kNoneMatch) {
      entries.e = entries.b;
      break;
    }
    if (cast.value.validity == CastFilterValueResult::kAllMatch) {
      one_key = false;
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
  if (searches_.empty() || entries.empty()) {
    return base::Span<const uint32_t>(entries.b, entries.e);
  }
  if (one_key) {
    // The entries are rows in order, so only the rows between the first and
    // the last need searching.
    Range range =
        searches_.Find(*s.context, {*entries.b, *(entries.e - 1) + 1});
    const uint32_t* b = std::lower_bound(entries.b, entries.e, range.b);
    const uint32_t* e = std::lower_bound(b, entries.e, range.e);
    return base::Span<const uint32_t>(b, e);
  }
  auto rows = static_cast<uint32_t>(permutation_->size());
  Range range = searches_.Find(*s.context, {0, rows});
  s.in_range.clear();
  for (const uint32_t* it = entries.b; it != entries.e; ++it) {
    if (*it >= range.b && *it < range.e) {
      s.in_range.push_back(*it);
    }
  }
  return base::Span<const uint32_t>(s.in_range.data(),
                                    s.in_range.data() + s.in_range.size());
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
