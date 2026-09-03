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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ASSERT_TYPE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ASSERT_TYPE_H_

#include <cstdint>
#include <memory>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/type_set.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

using AssertTypeTarget = base::TypeSet<Int64, Double, String>;

// Converts a column whose values carry their own type into a column of a
// single type, failing on any row which disagrees.
//
// A source reading SQLite cannot promise a column's type, because a declared
// type in SQLite is not binding. AssertType is needed for variant columns. A
// flat column already of the requested type passes through unchanged; a flat
// integer column of any other type is copied into a widened one. Integers
// widen to a float only where the conversion is exact; nothing else converts.
class AssertType : public Operator {
 public:
  AssertType(uint32_t column, AssertTypeTarget type, std::string name);
  ~AssertType() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState&) const override;
  void Rewind(OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  struct State : OperatorState {
    ~State() override;
    // The converted column, holding values of the target type.
    ColumnChunk chunk;
    base::Status status = base::OkStatus();
  };

  // The buffer in `chunk` holding values of the target type.
  const void* Data(const ColumnChunk& chunk) const;
  bool Widen(const ColumnView&, uint32_t count, State&) const;

  uint32_t column_;
  AssertTypeTarget target_;
  // `target_` as a StorageType, which is what a column carries.
  StorageType type_;
  // Used in the error message when a row disagrees.
  std::string name_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ASSERT_TYPE_H_
