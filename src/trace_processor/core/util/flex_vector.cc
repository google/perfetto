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

#include "src/trace_processor/core/util/flex_vector.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core::internal {

GrownFlexVector GrowFlexVector(void* data,
                               uint64_t used_bytes,
                               uint64_t capacity,
                               uint64_t min_capacity,
                               size_t width,
                               size_t align) {
  using Bytes = FlexVector<uint8_t>;
  auto grown = static_cast<uint64_t>(static_cast<double>(capacity) *
                                     Bytes::kGrowthFactor);
  uint64_t target =
      base::AlignUp(std::max(min_capacity, grown), Bytes::kCapacityMultiple);
  uint64_t bytes = target * width;
  size_t alignment = SlabAlignment(bytes, align);
  void* out = base::AlignedAlloc(alignment, bytes);
  if (used_bytes) {
    memcpy(out, data, used_bytes);
  }
  base::AlignedFree(data);
  return {out, target};
}

}  // namespace perfetto::trace_processor::core::internal
