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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_TRANSIENT_COLUMN_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_TRANSIENT_COLUMN_H_

#include <cstdint>
#include <cstring>
#include <memory>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

inline constexpr uint32_t kMaxBatchRows = 2048;

class ValidityView;

// One transient column in a row batch. A selection can turn any non-constant
// column into a dictionary column without changing its underlying
// representation.
class TransientColumn {
 public:
  enum class Kind : uint8_t {
    kFlat,
    kSequence,
  };

  TransientColumn() = default;

  static TransientColumn Reference(StorageType type,
                                   const void* data,
                                   const BitVector* validity = nullptr) {
    TransientColumn vector;
    vector.type_ = type;
    if (type.Is<Id>()) {
      PERFETTO_DCHECK(data == nullptr && validity == nullptr);
      vector.kind_ = Kind::kSequence;
      return vector;
    }
    vector.kind_ = Kind::kFlat;
    vector.data_ = data;
    vector.validity_ = validity;
    return vector;
  }

  Kind kind() const { return kind_; }
  StorageType type() const { return type_; }
  RowSelection selection() const { return selection_; }
  // The values this column reads from, before its row view is applied.
  const void* data() const { return data_; }

  // Composes `selection` with the current logical-to-physical row view.
  void Slice(RowSelection selection, uint32_t count);

  // Points this column at physical rows the caller owns. Nothing is copied, so
  // `rows` must outlive the batch's current contents.
  void SetBorrowedRows(Span<const uint32_t> rows) {
    owned_selection_.reset();
    selection_ = RowSelection::Indices(rows);
  }

  // Takes over the row view `other` just computed. Columns of one batch that
  // shared a view before slicing still share it afterwards, so only the first
  // of them has to compose it.
  void AdoptSelection(const TransientColumn& other) {
    selection_ = other.selection_;
    owned_selection_ = other.owned_selection_;
  }

 private:
  Kind kind_ = Kind::kFlat;
  StorageType type_{Id{}};
  RowSelection selection_ = RowSelection::Range();
  // Owns composed dictionary indices. Copies of a column share this immutable
  // auxiliary buffer.
  std::shared_ptr<const FlexVector<uint32_t>> owned_selection_;
  const void* data_ = nullptr;
  const BitVector* validity_ = nullptr;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TRANSIENT_COLUMN_H_
