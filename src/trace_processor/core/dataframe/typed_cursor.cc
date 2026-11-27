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

#include "src/trace_processor/core/dataframe/typed_cursor.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/sort_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/logical_plan.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/dataframe_windows.h"
#include "src/trace_processor/core/exec/empty_source.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/exec/index_scan.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/param_value.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/exec/sort.h"
#include "src/trace_processor/core/exec/sorted_scan.h"
#include "src/trace_processor/core/util/arena.h"

namespace perfetto::trace_processor::core::dataframe {
namespace {

template <typename... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

PERFETTO_NO_INLINE void AppendRows(std::vector<uint32_t>& rows,
                                   const exec::RowBatch& batch) {
  uint32_t n = batch.size();
  size_t old = rows.size();
  rows.resize(old + n);
  uint32_t* dst = rows.data() + old;
  const exec::Selection& sel = batch.selection();
  const exec::ColumnView& id_view = batch.column(0);
  if (id_view.kind() == exec::ColumnView::Kind::kSequence) {
    uint32_t base = id_view.start();
    if (sel.prefix()) {
      std::iota(dst, dst + n, base);
    } else {
      const uint16_t* idx = sel.indices();
      for (uint32_t i = 0; i < n; ++i) {
        dst[i] = base + idx[i];
      }
    }
    return;
  }
  PERFETTO_DCHECK(id_view.kind() == exec::ColumnView::Kind::kFlat);
  const auto* data =
      static_cast<const uint32_t*>(id_view.data()) + id_view.start();
  if (sel.prefix()) {
    memcpy(dst, data, uint64_t{n} * sizeof(uint32_t));
  } else {
    const uint16_t* idx = sel.indices();
    for (uint32_t i = 0; i < n; ++i) {
      dst[i] = data[idx[i]];
    }
  }
}

}  // namespace

struct TypedCursor::ExecState {
  struct Binding {
    exec::ParamCaster<Fetcher> cast;
    uint32_t value;
    uint32_t param;
  };

  // Declared in dependency order so ~ExecState destroys:
  //   scratch -> context (owns op_state) -> arena (owns operators/pipelines)
  //   -> pipelines -> id_column
  Column id_column{Storage{Storage::Id{0}}, NullStorage{NullStorage::NonNull{}},
                   IdSorted{}, NoDuplicates{}};
  exec::Pipelines pipelines;
  core::Arena arena;
  exec::Context context;
  const exec::Source* root = nullptr;
  exec::OperatorState* op_state = nullptr;
  const StringPool* pool = nullptr;
  std::vector<Binding> bindings;
  std::vector<exec::ParamValue> params;
  std::vector<uint32_t> rows;

  // Kept last because Selection embeds a 4 KB array; keeping hot fields
  // before it keeps them in the first two cache lines of ExecState.
  exec::RowBatch scratch;
};

TypedCursor::TypedCursor(const Dataframe* dataframe,
                         std::vector<FilterSpec> filter_specs,
                         std::vector<SortSpec> sort_specs,
                         bool mut)
    : dataframe_(dataframe),
      filter_specs_(std::move(filter_specs)),
      sort_specs_(std::move(sort_specs)),
      mutable_(mut) {
  filter_values_.resize(filter_specs_.size());
  filter_value_list_states_.resize(filter_specs_.size());

  std::vector<uint32_t> cols;
  cols.reserve(filter_specs_.size() + sort_specs_.size());
  for (const auto& spec : filter_specs_) {
    cols.push_back(spec.col);
  }
  for (const auto& spec : sort_specs_) {
    cols.push_back(spec.col);
  }
  std::sort(cols.begin(), cols.end());
  cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
  mutation_columns_ = core::Slab<uint32_t>::Alloc(cols.size());
  for (size_t i = 0; i < cols.size(); ++i) {
    mutation_columns_[i] = cols[i];
  }
}

TypedCursor::~TypedCursor() = default;

void TypedCursor::Reset() {
  std::fill(filter_values_.begin(), filter_values_.end(), nullptr);
  std::fill(filter_value_list_states_.begin(), filter_value_list_states_.end(),
            FilterValueListState{});
  exec_.reset();
  prepared_finalized_ = false;
  last_execution_generation_ = std::numeric_limits<uint64_t>::max();
  last_execution_mutation_count_ = std::numeric_limits<uint64_t>::max();
  SetRange(0, 0);
}

void TypedCursor::ExecuteUnchecked() {
  if (PERFETTO_UNLIKELY(NeedsPrepareCursor())) {
    PrepareCursorInternal();
  }
  ExecState& e = *exec_;
  Fetcher fetcher{{}, filter_values_.data(), filter_value_list_states_.data()};
  for (const ExecState::Binding& b : e.bindings) {
    b.cast(fetcher, b.value, *e.pool, &e.params[b.param]);
  }
  exec::Restart(*e.root, *e.op_state);

  exec::RowBatch* batch = e.root->Next(e.scratch, *e.op_state);
  if (PERFETTO_UNLIKELY(!batch)) {
    SetRange(0, 0);
    return;
  }
  if (PERFETTO_LIKELY(batch->last())) {
    uint32_t n = batch->size();
    if (PERFETTO_UNLIKELY(n == 0)) {
      SetRange(0, 0);
      return;
    }
    const exec::Selection& sel = batch->selection();
    const exec::ColumnView& id_view = batch->column(0);
    if (id_view.kind() == exec::ColumnView::Kind::kSequence) {
      if (sel.prefix()) {
        SetRange(id_view.start(), id_view.start() + n);
        return;
      }
      if (n == 1) {
        uint32_t r = id_view.start() + sel.indices()[0];
        SetRange(r, r + 1);
        return;
      }
    } else if (id_view.kind() == exec::ColumnView::Kind::kFlat &&
               sel.prefix()) {
      const auto* data =
          static_cast<const uint32_t*>(id_view.data()) + id_view.start();
      SetSlice(data, data + n);
      return;
    }
  }
  DrainBatches(batch);
}

void TypedCursor::DrainBatches(exec::RowBatch* first) {
  ExecState& e = *exec_;
  e.rows.clear();
  bool coalescing = true;
  uint32_t range_b = 0;
  uint32_t range_e = 0;

  for (exec::RowBatch* batch = first; batch != nullptr;
       batch = e.root->Next(e.scratch, *e.op_state)) {
    uint32_t n = batch->size();
    if (n != 0) {
      const exec::Selection& sel = batch->selection();
      const exec::ColumnView& id_view = batch->column(0);
      bool contiguous_seq =
          id_view.kind() == exec::ColumnView::Kind::kSequence && sel.prefix();
      if (coalescing && contiguous_seq &&
          (range_b == range_e || id_view.start() == range_e)) {
        if (range_b == range_e) {
          range_b = id_view.start();
        }
        range_e = id_view.start() + n;
      } else {
        if (coalescing) {
          coalescing = false;
          if (range_b != range_e) {
            e.rows.resize(range_e - range_b);
            std::iota(e.rows.begin(), e.rows.end(), range_b);
          }
        }
        AppendRows(e.rows, *batch);
      }
    }
    if (batch->last()) {
      break;
    }
  }

  if (coalescing) {
    SetRange(range_b, range_e);
  } else {
    SetSlice(e.rows.data(), e.rows.data() + e.rows.size());
  }
}

void TypedCursor::PrepareCursorInternal() {
  // Reset value_index before planning (re-planning on mutation/replacement).
  for (auto& spec : filter_specs_) {
    spec.value_index = std::nullopt;
  }
  auto plan_or = dataframe_->PlanQueryLogical(filter_specs_, {}, sort_specs_,
                                              {}, /*cols_used=*/0);
  PERFETTO_CHECK(plan_or.ok());
  const LogicalPlan& logical = *plan_or;

  last_execution_generation_ = dataframe_->replacement_generation_;
  last_execution_mutation_count_ = GetMutations();
  prepared_finalized_ = dataframe_->finalized_;

  auto state = std::make_unique<ExecState>();
  state->pool = dataframe_->string_pool_;
  core::Arena& arena = state->arena;

  bool is_empty = std::any_of(
      logical.ops.begin(), logical.ops.end(), [](const logical::Operation& op) {
        return std::holds_alternative<logical::Empty>(op);
      });
  if (is_empty) {
    state->root = arena.New<exec::EmptySource>();
    state->op_state = state->root->MakeState(state->context);
    exec_ = std::move(state);
    SetRange(0, 0);
    return;
  }

  // Map LogicalPlan's value_index back to the caller's source_index.
  std::vector<uint32_t> source_of_value_index(logical.filter_value_count, 0);
  for (const auto& spec : filter_specs_) {
    if (spec.value_index.has_value()) {
      source_of_value_index[*spec.value_index] = spec.source_index;
    }
  }

  // Column 0 of every batch is always the row index (synthetic Id column).
  state->id_column =
      Column{Storage{Storage::Id{dataframe_->row_count_}},
             NullStorage{NullStorage::NonNull{}}, IdSorted{}, NoDuplicates{}};
  std::vector<const Column*> batch_cols = {&state->id_column};
  std::vector<uint32_t> batch_col_of(dataframe_->columns_.size(),
                                     std::numeric_limits<uint32_t>::max());
  auto ensure_batch_col = [&](uint32_t df_col) -> uint32_t {
    if (batch_col_of[df_col] == std::numeric_limits<uint32_t>::max()) {
      batch_col_of[df_col] = static_cast<uint32_t>(batch_cols.size());
      batch_cols.push_back(dataframe_->column_ptrs_[df_col]);
    }
    return batch_col_of[df_col];
  };

  auto add_param = [&](StorageType storage, Op op,
                       uint32_t value_index) -> uint32_t {
    StorageType cast_type = storage.Is<Id>() ? StorageType{Uint32{}} : storage;
    auto param = static_cast<uint32_t>(state->params.size());
    state->params.emplace_back();
    state->bindings.push_back(
        {op.Is<In>() ? exec::GetParamListCaster<Fetcher>(cast_type)
                     : exec::GetParamCaster<Fetcher>(cast_type, op),
         source_of_value_index[value_index], param});
    return param;
  };

  std::optional<uint32_t> index_idx;
  std::vector<exec::IndexScan::Equality> index_eqs;
  std::vector<exec::SortedSearches::Search> sorted_searches;
  std::vector<exec::Filter::Condition> filter_conds;
  exec::ReadDirection direction = exec::ReadDirection::kForwards;
  std::vector<SortSpec> sort_keys;

  auto visit_op = Overloaded{
      [](const logical::Scan&) {},
      [](const logical::Output&) {},
      [](const logical::Empty&) { PERFETTO_FATAL("Handled above"); },
      [](const logical::Distinct&) { PERFETTO_FATAL("Unexpected Distinct"); },
      [](const logical::MinMax&) { PERFETTO_FATAL("Unexpected MinMax"); },
      [](const logical::Limit&) { PERFETTO_FATAL("Unexpected Limit"); },
      [&](const logical::IndexFilter& idx) {
        // IndexScan handles leading Eq predicates; any trailing In predicate
        // falls through to a Filter condition.
        for (const auto& p : idx.predicates) {
          PERFETTO_CHECK(p.value_index.has_value());
          uint32_t param = add_param(p.storage, p.op, *p.value_index);
          if (p.op.Is<Eq>() && !p.storage.Is<Id>() &&
              (!index_idx || *index_idx == idx.index)) {
            index_idx = idx.index;
            index_eqs.push_back({dataframe_->column_ptrs_[p.col], param});
          } else {
            filter_conds.push_back({ensure_batch_col(p.col), p.op, param});
          }
        }
      },
      [&](const logical::Filter& f) {
        uint32_t param = 0;
        if (f.value_index && !f.op.IsAnyOf<NullOp>()) {
          param = add_param(f.storage, f.op, *f.value_index);
        }
        auto add_sorted = [&](exec::SortedSearches::Search::Kind kind) {
          sorted_searches.push_back(
              {dataframe_->column_ptrs_[f.col], kind, f.op, param});
        };
        std::visit(
            Overloaded{
                [&](logical::BinarySearch) {
                  add_sorted(exec::SortedSearches::Search::Kind::kBinarySearch);
                },
                [&](logical::SetIdSortedSearch) {
                  add_sorted(exec::SortedSearches::Search::Kind::kSetIdEq);
                },
                [&](logical::SmallValueLookup) {
                  add_sorted(exec::SortedSearches::Search::Kind::kSmallValueEq);
                },
                [&](logical::RangeScan) {
                  filter_conds.push_back(
                      {ensure_batch_col(f.col), f.op, param});
                },
                [&](logical::IndexListScan) {
                  filter_conds.push_back(
                      {ensure_batch_col(f.col), f.op, param});
                },
            },
            f.strategy);
      },
      [&](const logical::Reverse&) {
        direction = exec::ReadDirection::kBackwards;
      },
      [&](const logical::Sort& s) { sort_keys = s.keys; },
  };
  for (const logical::Operation& op : logical.ops) {
    std::visit(visit_op, op);
  }

  std::vector<exec::Pipeline::Step> steps;
  if (!filter_conds.empty()) {
    steps.emplace_back(arena.New<exec::Filter>(
        arena.CopySpan<exec::Filter::Condition>(filter_conds), state->pool));
  }

  exec::Sort* sort = nullptr;
  if (!sort_keys.empty()) {
    auto keys =
        arena.NewSpan<exec::SortKey>(static_cast<uint32_t>(sort_keys.size()));
    for (uint32_t i = 0; i < sort_keys.size(); ++i) {
      keys[i] = {ensure_batch_col(sort_keys[i].col),
                 sort_keys[i].direction == SortDirection::kDescending};
    }
    sort = arena.New<exec::Sort>(exec::SortSpec{keys, state->pool});
    auto unread = arena.NewSpan<bool>(static_cast<uint32_t>(batch_cols.size()));
    std::fill(unread.begin(), unread.end(), true);
    unread[0] = false;
    sort->SetUnreadColumns(unread);
  }

  auto cols_span = arena.CopySpan<const Column*>(batch_cols);
  const exec::Source* source = nullptr;
  if (index_idx.has_value() && !index_eqs.empty()) {
    // IndexScan reads in index order (forwards only).
    source = arena.New<exec::IndexScan>(
        arena, cols_span, dataframe_->indexes_[*index_idx],
        arena.CopySpan<exec::IndexScan::Equality>(index_eqs),
        arena.CopySpan<exec::SortedSearches::Search>(sorted_searches),
        state->pool);
  } else if (!sorted_searches.empty()) {
    source = arena.New<exec::SortedScan>(
        arena, cols_span, dataframe_->row_count_,
        arena.CopySpan<exec::SortedSearches::Search>(sorted_searches),
        state->pool, direction);
  } else {
    source = arena.New<exec::DataframeScan>(cols_span, dataframe_->row_count_,
                                            direction);
  }

  if (sort) {
    auto* feed = arena.New<exec::Pipeline>(arena, *source, steps);
    steps.clear();
    uint32_t id = state->pipelines.Add(feed, sort);
    source = arena.New<exec::Served>(id, *sort);
    state->context.SetPipelines(&state->pipelines);
  }

  state->root =
      steps.empty() ? source : arena.New<exec::Pipeline>(arena, *source, steps);
  state->context.SetParams(state->params);
  state->op_state = state->root->MakeState(state->context);
  exec_ = std::move(state);
  SetRange(0, 0);
}

}  // namespace perfetto::trace_processor::core::dataframe
