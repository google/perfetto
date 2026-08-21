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
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

// Converts a column whose values carry their own type into a column of a
// single type, failing on any row which disagrees.
//
// A source reading SQLite cannot promise a column's type, because a declared
// type in SQLite is not binding: an INTEGER column holds text if something
// puts text in it. So anything downstream which needs a typed column has to
// come through here. An integer widens to a float where the conversion is
// exact; nothing else converts.
class AssertType : public Operator {
 public:
  AssertType(uint32_t column, StorageType type, std::string name);
  ~AssertType() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  struct Values {
    FlexVector<int64_t> ints;
    FlexVector<double> doubles;
    FlexVector<StringPool::Id> strings;
    BitVector validity;
  };
  struct State : OperatorState {
    ~State() override;
    std::shared_ptr<Values> values = std::make_shared<Values>();
    base::Status status = base::OkStatus();
  };

  void Widen(const ColumnView&, uint32_t count, Values&) const;

  uint32_t column_;
  StorageType type_;
  // Used in the error message when a row disagrees.
  std::string name_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ASSERT_TYPE_H_
