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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_SORT_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_SORT_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

struct SortSpec {
  struct Key {
    uint32_t column = 0;
    bool descending = false;
  };
  std::vector<Key> keys;
};

// Stably sorts rows by their keys. Nulls sort as in SQLite: first ascending,
// last descending.
// TODO(lalitm): support string keys, which needs the string pool for ranks.
class Sort : public Breaker {
 public:
  explicit Sort(SortSpec);
  ~Sort() override;

 private:
  struct State : Breaker::State {
    ~State() override;
    void Reset() override;

    std::vector<std::optional<RowLayout::Type>> types;
    RowLayout layout;
    FlexVector<uint8_t> keys;
    RowStore rows;
    FlexVector<uint32_t> order;
    uint32_t emitted = 0;
  };

  std::unique_ptr<Breaker::State> CreateState() const override;
  bool Consume(const RowBatch& in, Breaker::State& state) const override;
  bool Finalize(Breaker::State& state) const override;
  bool Serve(RowBatch& out, Breaker::State& state) const override;

  SortSpec spec_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_SORT_H_
