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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_OWNED_COLUMN_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_OWNED_COLUMN_H_

#include <cstdint>
#include <variant>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

// A copy of a column's values, kept by whoever needs them to outlive the
// storage they were read from.
//
// A column view is a view: it costs pointers, not values, and a batch built
// from one is only good for as long as what it points at stands still. That
// covers a source reading durable storage and does not cover a source which
// materialises into buffers it refills, so whether to copy cannot be decided
// once for the model. It is decided per column, by the operator, which is the
// only thing that knows how long it needs the values for.
//
// The copy is dense from row zero, so an operator which owns its input is also
// an operator which can add a computed column to it without the two ending up
// in different index spaces.
class OwnedColumn {
 public:
  // Copies `count` values out of `column`, resolved through its row view,
  // replacing whatever was here. An Id column materialises as the row numbers
  // it stood for.
  void Fill(const ColumnView& column, uint32_t count);

  // The same, onto the end of what is already here, for building one column
  // out of a stream of chunks.
  void Append(const ColumnView& column, uint32_t count);

  // Forgets the values, keeping the buffers.
  void Clear() { size_ = 0; }

  uint32_t size() const { return size_; }

  // A view of what was copied.
  ColumnView View() const;

 private:
  using Values = std::variant<FlexVector<uint32_t>,
                              FlexVector<int32_t>,
                              FlexVector<int64_t>,
                              FlexVector<double>,
                              FlexVector<StringPool::Id>>;

  StorageType type_{Uint32{}};
  Values values_{FlexVector<uint32_t>()};
  BitVector validity_;
  uint32_t size_ = 0;
  bool nullable_ = false;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_OWNED_COLUMN_H_
