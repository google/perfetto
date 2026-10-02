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

#include "src/trace_processor/core/util/column_vector.h"

#include <algorithm>

#include "perfetto/ext/base/utils.h"

namespace perfetto::trace_processor::core {

ColumnVectorBacking::~ColumnVectorBacking() = default;

uint64_t ComputeColumnVectorCapacity(uint64_t current, uint64_t requested) {
  constexpr uint64_t kCapacityMultiple = 64;
  uint64_t grown = current + current / 2;
  return base::AlignUp(
      std::max({requested, grown, kCapacityMultiple}), kCapacityMultiple);
}

}  // namespace perfetto::trace_processor::core
