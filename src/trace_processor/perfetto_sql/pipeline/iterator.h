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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_ITERATOR_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_ITERATOR_H_

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/param_value.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/variant.h"

namespace perfetto::trace_processor::pipeline {

class BatchIterator;
class PhysicalPlan;
class PreparedStatement;
class RowIterator;

namespace internal {

struct BatchState;
struct RowState;

template <typename Callback>
PERFETTO_ALWAYS_INLINE void EmitString(const StringPool* pool,
                                       StringPool::Id id,
                                       Callback& cb) {
  if (id.is_null()) {
    cb.OnCell(nullptr);
    return;
  }
  cb.OnCell(pool->Get(id));
}

template <typename Callback>
PERFETTO_ALWAYS_INLINE void DispatchStored(core::StorageType type,
                                           const void* data,
                                           uint32_t idx,
                                           const StringPool* pool,
                                           Callback& cb) {
  switch (type.index()) {
    case core::StorageType::GetTypeIndex<core::Id>():
    case core::StorageType::GetTypeIndex<core::Uint32>():
      cb.OnCell(static_cast<const uint32_t*>(data)[idx]);
      return;
    case core::StorageType::GetTypeIndex<core::Int32>():
      cb.OnCell(static_cast<const int32_t*>(data)[idx]);
      return;
    case core::StorageType::GetTypeIndex<core::Int64>():
      cb.OnCell(static_cast<const int64_t*>(data)[idx]);
      return;
    case core::StorageType::GetTypeIndex<core::Double>():
      cb.OnCell(static_cast<const double*>(data)[idx]);
      return;
    case core::StorageType::GetTypeIndex<core::String>():
      EmitString(pool, static_cast<const StringPool::Id*>(data)[idx], cb);
      return;
    default:
      PERFETTO_FATAL("Unexpected storage type");
  }
}

template <typename Callback>
PERFETTO_ALWAYS_INLINE void ReadCell(const core::exec::ColumnView& view,
                                     uint32_t row,
                                     const StringPool* pool,
                                     Callback&& cb) {
  if (PERFETTO_LIKELY(view.kind() == core::exec::ColumnView::Kind::kFlat)) {
    if (!view.IsValid(row)) {
      cb.OnCell(nullptr);
      return;
    }
    DispatchStored(view.type(), view.data(), view.Index(row), pool, cb);
    return;
  }
  switch (view.kind()) {
    case core::exec::ColumnView::Kind::kSequence:
      if (!view.IsValid(row)) {
        cb.OnCell(nullptr);
        return;
      }
      cb.OnCell(view.Index(row));
      return;
    case core::exec::ColumnView::Kind::kSparse: {
      uint32_t slot = core::filter::IndexToStorageIndex<core::SparseNull>(
          view.Index(row), view.validity(), view.popcount());
      if (slot == std::numeric_limits<uint32_t>::max()) {
        cb.OnCell(nullptr);
        return;
      }
      if (!view.sparse_data()) {
        cb.OnCell(slot);
        return;
      }
      DispatchStored(view.type(), view.sparse_data(), slot, pool, cb);
      return;
    }
    case core::exec::ColumnView::Kind::kVariant: {
      using core::exec::Variant;
      Variant cell = view.At<Variant>(row);
      switch (cell.type) {
        case Variant::Type::kNull:
          cb.OnCell(nullptr);
          return;
        case Variant::Type::kInt64:
          cb.OnCell(cell.AsInt64());
          return;
        case Variant::Type::kDouble:
          cb.OnCell(cell.AsDouble());
          return;
        case Variant::Type::kString:
          EmitString(pool, cell.AsString(), cb);
          return;
      }
      PERFETTO_FATAL("Unexpected variant type");
    }
    case core::exec::ColumnView::Kind::kFlat:
      PERFETTO_FATAL("Unreachable");
  }
}

struct BoundParamSpec {
  core::StorageType type;
  core::Op op;
  uint32_t value_index;
  uint32_t param_index;
  bool is_external;
};

struct ParamBindingLayout {
  const StringPool* pool = nullptr;
  std::vector<core::exec::ParamValue> initial_params;
  std::vector<BoundParamSpec> specs;
};

ParamBindingLayout DescribeParams(const PhysicalPlan& plan);
ParamBindingLayout DescribeParams(const BatchState& state);
ParamBindingLayout DescribeParams(const RowState& state);

}  // namespace internal

// -----------------------------------------------------------------------------
// Parameter binding
// -----------------------------------------------------------------------------

// Non-owning view of bound parameters returned by `ParamBindings::Bind()` and
// passed into `BatchIterator::Open()` / `RowIterator::Open()`. Valid until the
// next `Bind()` call or destruction of the originating `ParamBindings`.
class BoundParams {
 public:
  BoundParams() = default;

 private:
  template <typename Fetcher>
  friend class ParamBindings;
  friend class BatchIterator;
  friend class RowIterator;

  explicit BoundParams(base::Span<const core::exec::ParamValue> params)
      : params_(params) {}

  base::Span<const core::exec::ParamValue> params_;
};

// Reusable parameter binder for an iterator. Constant literals are cast once on
// construction; external literals (`Literal::External`) are cast once via
// `SetExternalLiterals()`; per-run bound params (`Literal::Bound`) are cast on
// each `Bind()`.
template <typename Fetcher>
class ParamBindings {
 public:
  ParamBindings() = default;

  // For tests running directly on a PhysicalPlan without an iterator.
  explicit ParamBindings(const PhysicalPlan& plan)
      : ParamBindings(internal::DescribeParams(plan)) {}

  // Casts statement-level external literals (`Literal::External`). Call once
  // before the first run when the statement uses external literals.
  void SetExternalLiterals(Fetcher& fetcher) {
    for (const Binding& b : external_literals_) {
      b.cast(fetcher, b.value_index, *pool_, &params_[b.param_index]);
      if (b.glob) {
        core::exec::AttachGlob(&params_[b.param_index]);
      }
    }
  }

  // Casts per-run bound parameters (`Literal::Bound`) for the next Open().
  BoundParams Bind(Fetcher& fetcher) {
    for (const Binding& b : bindings_) {
      b.cast(fetcher, b.value_index, *pool_, &params_[b.param_index]);
    }
    return BoundParams(params_);
  }

  // Raw span for low-level callers (e.g. core::exec::RowCursor).
  base::Span<const core::exec::ParamValue> BindSpan(Fetcher& fetcher) {
    return Bind(fetcher).params_;
  }

 private:
  friend class BatchIterator;
  friend class RowIterator;

  explicit ParamBindings(internal::ParamBindingLayout layout)
      : pool_(layout.pool), params_(std::move(layout.initial_params)) {
    for (const internal::BoundParamSpec& s : layout.specs) {
      Binding binding{s.op.Is<core::In>()
                          ? core::exec::GetParamListCaster<Fetcher>(s.type)
                          : core::exec::GetParamCaster<Fetcher>(s.type, s.op),
                      s.value_index, s.param_index, s.op.Is<core::Glob>()};
      if (s.is_external) {
        external_literals_.push_back(binding);
      } else {
        bindings_.push_back(binding);
      }
    }
  }

  struct Binding {
    core::exec::ParamCaster<Fetcher> cast;
    uint32_t value_index;
    uint32_t param_index;
    bool glob;
  };

  const StringPool* pool_ = nullptr;
  std::vector<core::exec::ParamValue> params_;
  std::vector<Binding> bindings_;
  std::vector<Binding> external_literals_;
};

// -----------------------------------------------------------------------------
// Batch-based execution API (default)
// -----------------------------------------------------------------------------

// Zero-copy view of one output batch from BatchIterator::Next().
// Valid only until the next Next(), Open(), or destruction of the iterator.
class Batch {
 public:
  uint32_t row_count() const { return batch_->size(); }
  uint32_t column_count() const { return column_count_; }

  // Maps logical row `r` in `[0, row_count())` to its index in column views.
  const core::exec::Selection& selection() const { return batch_->selection(); }

  const core::exec::ColumnView& column(uint32_t output_col) const {
    PERFETTO_DCHECK(output_col < column_count_);
    return batch_->column(column_indices_[output_col]);
  }

  PERFETTO_ALWAYS_INLINE bool IsValid(uint32_t output_col, uint32_t row) const {
    PERFETTO_DCHECK(output_col < column_count_);
    return batch_->IsValid(column_indices_[output_col], row);
  }

  template <typename T>
  PERFETTO_ALWAYS_INLINE T Value(uint32_t output_col, uint32_t row) const {
    PERFETTO_DCHECK(output_col < column_count_);
    return batch_->Value<T>(column_indices_[output_col], row);
  }

  // Invokes `callback.OnCell(...)` with the value (or nullptr) at `(col, row)`.
  template <typename Callback>
  PERFETTO_ALWAYS_INLINE void Cell(uint32_t output_col,
                                   uint32_t row,
                                   Callback&& callback) const {
    PERFETTO_DCHECK(output_col < column_count_);
    const core::exec::ColumnView& view =
        batch_->column(column_indices_[output_col]);
    uint32_t batch_row = batch_->selection()[row];
    internal::ReadCell(view, batch_row, pool_,
                       std::forward<Callback>(callback));
  }

 private:
  friend class BatchIterator;

  Batch(const core::exec::RowBatch* batch,
        const uint32_t* column_indices,
        uint32_t column_count,
        const StringPool* pool)
      : batch_(batch),
        column_indices_(column_indices),
        column_count_(column_count),
        pool_(pool) {}

  const core::exec::RowBatch* batch_ = nullptr;
  const uint32_t* column_indices_ = nullptr;
  uint32_t column_count_ = 0;
  const StringPool* pool_ = nullptr;
};

// Executes a PreparedStatement and yields zero-copy batches.
//
// Created via `PreparedStatement::Execute(ctx)`. Statements without bound
// parameters can call `Next()` immediately; call `Open()` / `Open(params)` to
// rewind or supply bound parameters. Check `status()` after `Next()` returns
// `std::nullopt`.
class BatchIterator {
 public:
  BatchIterator(BatchIterator&&) noexcept;
  BatchIterator& operator=(BatchIterator&&) noexcept;
  BatchIterator(const BatchIterator&) = delete;
  BatchIterator& operator=(const BatchIterator&) = delete;
  ~BatchIterator();

  void Open();
  void Open(BoundParams params);

  // Returns the next non-empty Batch, or `std::nullopt` on EOF or error.
  std::optional<Batch> Next();

  PERFETTO_ALWAYS_INLINE const base::Status& status() const {
    return context_->error();
  }

  uint32_t column_count() const {
    return static_cast<uint32_t>(column_indices_.size());
  }
  const std::string& column_name(uint32_t output_col) const;

  template <typename Fetcher>
  ParamBindings<Fetcher> MakeBindings() const {
    return ParamBindings<Fetcher>(internal::DescribeParams(*owned_));
  }

 private:
  friend class PreparedStatement;

  explicit BatchIterator(std::unique_ptr<PhysicalPlan> plan);

  std::unique_ptr<internal::BatchState> owned_;
  const core::exec::Source* source_ = nullptr;
  const core::exec::Context* context_ = nullptr;
  const StringPool* pool_ = nullptr;
  std::vector<uint32_t> column_indices_;
  bool exhausted_ = false;
};

// -----------------------------------------------------------------------------
// Row-based execution API
// -----------------------------------------------------------------------------

// Executes a PreparedStatement one row at a time.
//
// Created via `PreparedStatement::ExecuteRows(ctx)`. Call `Open()` (or
// `Open(params)`) to position on the first row, `Next()` to advance, `Cell()`
// to read columns, and `status()` after `Open()`/`Next()` returns false.
class RowIterator {
 public:
  RowIterator(RowIterator&&) noexcept;
  RowIterator& operator=(RowIterator&&) noexcept;
  RowIterator(const RowIterator&) = delete;
  RowIterator& operator=(const RowIterator&) = delete;
  ~RowIterator();

  // Rewinds and moves to the first row. Returns false on EOF or error.
  bool Open();
  PERFETTO_ALWAYS_INLINE bool Open(BoundParams params) {
    return cursor_->Open(params.params_);
  }

  // Moves to the next row. Returns false on EOF or error.
  PERFETTO_ALWAYS_INLINE bool Next() { return cursor_->Next(); }

  PERFETTO_ALWAYS_INLINE bool eof() const { return cursor_->eof(); }

  PERFETTO_ALWAYS_INLINE const base::Status& status() const {
    return cursor_->status();
  }

  uint32_t column_count() const {
    return static_cast<uint32_t>(column_indices_.size());
  }
  const std::string& column_name(uint32_t output_col) const;

  // Invokes `callback.OnCell(...)` with the value (or nullptr) of `output_col`
  // at the current row. Requires `!eof()`.
  template <typename Callback>
  PERFETTO_ALWAYS_INLINE void Cell(uint32_t output_col,
                                   Callback&& callback) const {
    PERFETTO_DCHECK(!cursor_->eof());
    PERFETTO_DCHECK(output_col < column_indices_.size());
    internal::ReadCell(cursor_->batch().column(column_indices_[output_col]),
                       cursor_->batch_row(), pool_,
                       std::forward<Callback>(callback));
  }

  template <typename Fetcher>
  ParamBindings<Fetcher> MakeBindings() const {
    return ParamBindings<Fetcher>(internal::DescribeParams(*owned_));
  }

 private:
  friend class PreparedStatement;

  explicit RowIterator(std::unique_ptr<PhysicalPlan> plan);

  std::unique_ptr<internal::RowState> owned_;
  core::exec::RowCursor* cursor_ = nullptr;
  const StringPool* pool_ = nullptr;
  std::vector<uint32_t> column_indices_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_ITERATOR_H_
