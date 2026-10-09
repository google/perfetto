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
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {

// One column of a RowBatch: a window onto storage the batch does not own.
// Row `row` of the batch is the value at `start() + row`, and so is its bit
// in the validity: moving the window moves both.
class ColumnView {
 public:
  enum class Kind : uint8_t {
    kFlat,
    // No storage: the value is where it would be stored.
    kSequence,
    // `data()` is a Variant array and `type()` is meaningless.
    kVariant,
  };

  ColumnView() = default;

  static ColumnView Reference(StorageType type,
                              const void* data,
                              const BitVector* validity = nullptr,
                              uint32_t start = 0) {
    ColumnView view;
    view.type_ = type;
    view.kind_ = Kind::kFlat;
    if (type.Is<Id>()) {
      PERFETTO_DCHECK(data == nullptr);
      view.kind_ = Kind::kSequence;
    }
    view.data_ = data;
    view.validity_ = validity;
    view.start_ = start;
    return view;
  }

  // A column carrying a type per row rather than one for the whole column.
  static ColumnView Variants(const Variant* data, uint32_t start = 0) {
    ColumnView view;
    view.kind_ = Kind::kVariant;
    view.data_ = data;
    view.start_ = start;
    return view;
  }

  Kind kind() const { return kind_; }
  StorageType type() const { return type_; }

  // Where the batch's rows start in `data()` and `validity()`.
  uint32_t start() const { return start_; }
  void set_start(uint32_t start) { start_ = start; }

  // Where batch row `row` is stored.
  PERFETTO_ALWAYS_INLINE uint32_t Index(uint32_t row) const {
    return start_ + row;
  }

  // The value of batch row `row`, which must hold one.
  template <typename T>
  PERFETTO_ALWAYS_INLINE T At(uint32_t row) const {
    uint32_t index = start_ + row;
    if constexpr (std::is_arithmetic_v<T>) {
      if (kind_ == Kind::kSequence) {
        return static_cast<T>(index);
      }
    }
    return static_cast<const T*>(data_)[index];
  }

  // Whether batch row `row` holds a value.
  PERFETTO_ALWAYS_INLINE bool IsValid(uint32_t row) const {
    return !validity_ || validity_->is_set(start_ + row);
  }

  // The values the window is onto.
  const void* data() const { return data_; }

  // Which stored values are there, or null when they all are.
  const BitVector* validity() const { return validity_; }

 private:
  Kind kind_ = Kind::kFlat;
  StorageType type_{Id{}};
  uint32_t start_ = 0;
  const void* data_ = nullptr;
  const BitVector* validity_ = nullptr;
};

// Whether two batches' views of a column can be combined. An implicit Id and
// a stored Uint32 hold the same values: gathering turns one into the other.
inline bool SameLogicalType(const ColumnView& a, const ColumnView& b) {
  auto is_uint32 = [](const ColumnView& view) {
    return view.kind() != ColumnView::Kind::kVariant &&
           (view.type().Is<Id>() || view.type().Is<Uint32>());
  };
  if (is_uint32(a) && is_uint32(b)) {
    return true;
  }
  return a.kind() == b.kind() &&
         (a.kind() == ColumnView::Kind::kVariant || a.type() == b.type());
}

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_COLUMN_VIEW_H_
