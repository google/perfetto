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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_COLUMN_VIEW_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_COLUMN_VIEW_H_

#include <cstdint>
#include <type_traits>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {

// One column of a RowBatch: a reference to storage the batch does not own,
// plus the row selection currently applied to it.
class ColumnView {
 public:
  enum class Kind : uint8_t {
    kFlat,
    kSequence,
    // `data()` is a Variant array and `type()` is meaningless.
    kVariant,
  };

  ColumnView() = default;

  static ColumnView Reference(StorageType type,
                              const void* data,
                              const BitVector* validity = nullptr) {
    ColumnView view;
    view.type_ = type;
    if (type.Is<Id>()) {
      PERFETTO_DCHECK(data == nullptr && validity == nullptr);
      view.kind_ = Kind::kSequence;
      return view;
    }
    view.kind_ = Kind::kFlat;
    view.data_ = data;
    view.validity_ = validity;
    return view;
  }

  // A column carrying a type per row rather than one for the whole column.
  static ColumnView Variants(const Variant* data) {
    ColumnView view;
    view.kind_ = Kind::kVariant;
    view.data_ = data;
    return view;
  }

  Kind kind() const { return kind_; }
  StorageType type() const { return type_; }
  RowSelection selection() const { return selection_; }

  // Composes `selection` with the selection already applied, materializing the
  // result into a block of `pool` if one is needed. The ordinals in
  // `selection` must be strictly increasing.
  void Slice(RowSelection selection, uint32_t count, SelectionPool& pool);

  // Points this column at physical rows the caller owns. Nothing is copied, so
  // `rows` has to outlive the batch's current contents.
  void SetBorrowedRows(Span<const uint32_t> rows) {
    selection_ = RowSelection::Indices(rows);
    block_ = nullptr;
  }

  // Forgets which batch's block this selection was composed into. A batch
  // adopting a column from another batch must call this, or narrowing the
  // column would write into the other batch's storage.
  void DisownBlock() { block_ = nullptr; }

  // Points this column at the run of physical rows starting at `offset`.
  void SetRange(uint32_t offset) {
    selection_ = RowSelection::Range(offset);
    block_ = nullptr;
  }

  // Takes over the selection `other` just composed. Columns sharing a
  // selection before a slice still share one afterwards, so only the first of
  // them has to do the work.
  void AdoptSelection(const ColumnView& other) {
    selection_ = other.selection_;
    block_ = other.block_;
  }

  // The value at logical row `row`, resolved through this column's own row
  // view. Reading is per column because a computed column added by an operator
  // sits in its own index space rather than its input's.
  template <typename T>
  PERFETTO_ALWAYS_INLINE T Value(uint32_t row) const {
    uint32_t index = selection_.GetIndex(row);
    if constexpr (std::is_arithmetic_v<T>) {
      // A sequence column has no storage: the value is the row it sits at.
      if (kind_ == Kind::kSequence) {
        return static_cast<T>(index);
      }
    }
    return static_cast<const T*>(data_)[index];
  }

  // The values this column reads from, before its selection is applied.
  const void* data() const { return data_; }

  // Which physical rows hold a value, or null when they all do.
  const BitVector* validity() const { return validity_; }

 private:
  Kind kind_ = Kind::kFlat;
  StorageType type_{Id{}};
  RowSelection selection_ = RowSelection::Range();
  // The batch block this column's selection was composed into, or null when
  // the selection points at storage the batch does not own. Columns sharing a
  // selection share the block behind it.
  uint32_t* block_ = nullptr;
  const void* data_ = nullptr;
  const BitVector* validity_ = nullptr;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_COLUMN_VIEW_H_
