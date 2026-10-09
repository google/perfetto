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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_KEY_ENCODER_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_KEY_ENCODER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

// Lays out rows' keys so rows with equal keys have equal bytes. Integers of
// every width are laid out as Int64 so they agree, and nulls agree with each
// other as under GROUP BY. A column must hold one type in every batch.
class KeyEncoder {
 public:
  // Returns the position in `columns` of one which cannot be a key or whose
  // type changed.
  std::optional<uint32_t> Encode(const RowBatch& batch,
                                 const std::vector<uint32_t>& columns);

  std::string_view Key(uint32_t row) const {
    return {reinterpret_cast<const char*>(bytes_.data()) +
                size_t{row} * layout_.stride(),
            layout_.stride()};
  }

 private:
  enum class Kind : uint8_t { kInteger, kDouble, kString };
  // Lays out one column's keys, for columns of one storage type.
  using Writer = void (*)(const ColumnView& column,
                          const Selection& selection,
                          RowLayout::Slot slot,
                          uint8_t* rows);

  // Checks each column can be a key of the kind it was before, laying the
  // keys out on the first batch, then writes them. Out of line: a batch whose
  // columns are shaped as the last batch's were needs none of it.
  std::optional<uint32_t> Classify(const RowBatch& batch,
                                   const std::vector<uint32_t>& columns);
  void WriteKeys(const RowBatch& batch, const std::vector<uint32_t>& columns);

  // What Encode() needs of one column, in 16 bytes. `shape` is its kind and
  // storage type as of the last batch classified.
  struct KeyColumn {
    uint16_t shape;
    RowLayout::Slot slot;
    Writer writer;
  };

  std::vector<KeyColumn> key_columns_;
  std::vector<Kind> kinds_;
  RowLayout layout_;
  FlexVector<uint8_t> bytes_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_KEY_ENCODER_H_
