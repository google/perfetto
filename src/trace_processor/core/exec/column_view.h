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

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {

// One column of a RowBatch: a reference to storage the batch does not own,
// plus the logical-to-physical row view currently applied.
class ColumnView {
 public:
  enum class Kind : uint8_t {
    kFlat,
    kSequence,
    // `data()` is a Variant array; `type()` means nothing.
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

  // A column whose type is per row.
  static ColumnView Variants(const Variant* data) {
    ColumnView view;
    view.kind_ = Kind::kVariant;
    view.data_ = data;
    return view;
  }

  Kind kind() const { return kind_; }
  StorageType type() const { return type_; }
  RowSelection selection() const { return selection_; }

  // Composes `selection` with the current logical-to-physical row view,
  // materializing the result into a block of `pool` if it needs one. The
  // ordinals in `selection` must be strictly increasing.
  void Slice(RowSelection selection, uint32_t count, SelectionPool& pool);

  // Points this column at physical rows the caller owns. Nothing is copied, so
  // `rows` must outlive the batch's current contents.
  void SetBorrowedRows(Span<const uint32_t> rows) {
    selection_ = RowSelection::Indices(rows);
    block_ = nullptr;
  }

  // Points this column at the run of physical rows starting at `offset`.
  void SetRange(uint32_t offset) {
    selection_ = RowSelection::Range(offset);
    block_ = nullptr;
  }

  // Takes over the row view `other` just computed. Columns of one batch that
  // shared a view before slicing still share it afterwards, so only the first
  // of them has to compose it.
  void AdoptSelection(const ColumnView& other) {
    selection_ = other.selection_;
    block_ = other.block_;
  }

  // The values this column reads from, before its row view is applied.
  const void* data() const { return data_; }

  // Which physical rows hold a value, or null when every row does.
  const BitVector* validity() const { return validity_; }

 private:
  Kind kind_ = Kind::kFlat;
  StorageType type_{Id{}};
  RowSelection selection_ = RowSelection::Range();
  // The batch's block this column's view was composed into, or null when the
  // view references other storage. Columns that share a view share the block
  // behind it.
  uint32_t* block_ = nullptr;
  const void* data_ = nullptr;
  const BitVector* validity_ = nullptr;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_COLUMN_VIEW_H_
