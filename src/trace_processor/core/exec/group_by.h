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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_GROUP_BY_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_GROUP_BY_H_

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/key_encoder.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/util/bump_allocator.h"

namespace perfetto::trace_processor::core::exec {

// Puts rows sharing their keys next to each other. Stable, so an order the
// input had holds within each group. Appends each row's group number as a
// Uint32 column, so what follows finds group boundaries without comparing
// keys.
class GroupBy : public Breaker {
 public:
  explicit GroupBy(std::vector<uint32_t> key_columns);
  ~GroupBy() override;

 private:
  struct State : Breaker::State {
    ~State() override;
    void Reset() override;

    KeyEncoder keys;
    // The keys `group_of` views.
    BumpAllocator group_keys;
    base::FlatHashMapV2<std::string_view, uint32_t> group_of;
    FlexVector<uint32_t> groups;
    RowStore rows;
    FlexVector<uint32_t> next;
    FlexVector<uint32_t> order;
    FlexVector<uint32_t> ordered_groups;
    uint32_t emitted = 0;
  };

  std::unique_ptr<Breaker::State> CreateState() const override;
  bool Consume(const RowBatch& in, Breaker::State& state) const override;
  bool Finalize(Breaker::State& state) const override;
  bool Serve(RowBatch& out, Breaker::State& state) const override;

  std::vector<uint32_t> key_columns_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_GROUP_BY_H_
