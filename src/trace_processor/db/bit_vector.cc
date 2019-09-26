/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/db/bit_vector.h"

namespace perfetto {
namespace trace_processor {

BitVector::BitVector() = default;

BitVector::BitVector(uint32_t count, bool value) {
  Resize(count, value);
}

BitVector::BitVector(std::vector<Block> blocks,
                     std::vector<uint32_t> counts,
                     uint32_t size)
    : size_(size), counts_(std::move(counts)), blocks_(std::move(blocks)) {}

BitVector BitVector::Copy() const {
  return BitVector(blocks_, counts_, size_);
}

}  // namespace trace_processor
}  // namespace perfetto
