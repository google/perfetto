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

#include "src/trace_processor/core/exec/run_vector.h"

#include "perfetto/base/compiler.h"

#include <cstdint>
#include <cstring>
#include <memory>

#include "src/trace_processor/core/exec/operator.h"

namespace perfetto::trace_processor::core::exec {

void RunBits::resize(OperatorState& owner, uint64_t size, bool value) {
  auto had = static_cast<uint32_t>(words_.size());
  words_.resize(owner, (size + 63) / 64);
  for (uint32_t w = had; w < words_.size(); ++w) {
    words_[w] = 0;
  }
  // Bits past the end stay unset, which push_back() relies on.
  if (size < size_) {
    if (size % 64) {
      words_[size / 64] &= (uint64_t{1} << (size % 64)) - 1;
    }
  } else if (value && size > size_) {
    // Bits past the old end are unset, so whole words can be set.
    uint64_t first = size_ / 64;
    uint64_t last = (size - 1) / 64;
    uint64_t below = (uint64_t{1} << (size_ % 64)) - 1;
    uint64_t kept = words_[first] & below;
    for (uint64_t w = first; w <= last; ++w) {
      words_[w] = ~uint64_t{0};
    }
    words_[first] = kept | ~below;
    if (size % 64) {
      words_[last] &= (uint64_t{1} << (size % 64)) - 1;
    }
  }
  size_ = size;
}

uint64_t RunBits::CountSetBits() const {
  uint64_t count = 0;
  for (uint64_t word : words_) {
    count += static_cast<uint64_t>(PERFETTO_POPCOUNT(word));
  }
  return count;
}

PERFETTO_NO_INLINE void RunMap::Grow(OperatorState& owner) {
  uint32_t had = tags_.size();
  std::unique_ptr<uint8_t[]> old_tags;
  std::unique_ptr<uint64_t[]> old_keys;
  std::unique_ptr<uint32_t[]> old_values;
  if (had) {
    old_tags.reset(new uint8_t[had]);
    old_keys.reset(new uint64_t[had]);
    old_values.reset(new uint32_t[had]);
    memcpy(old_tags.get(), tags_.data(), had);
    memcpy(old_keys.get(), keys_.data(), had * sizeof(uint64_t));
    memcpy(old_values.get(), values_.data(), had * sizeof(uint32_t));
  }
  // Quickly up to 1 MB, then by doubling, as FlatHashMap grows.
  uint64_t factor =
      uint64_t{had} * (sizeof(uint64_t) + sizeof(uint32_t)) < 1024 * 1024 ? 8
                                                                          : 2;
  uint64_t capacity = std::max<uint64_t>(had * factor, 1024);
  PERFETTO_CHECK(capacity <= UINT32_MAX);
  tags_.assign(owner, capacity, 0);
  keys_.clear();
  values_.clear();
  keys_.resize(owner, capacity);
  values_.resize(owner, capacity);
  size_ = 0;
  max_probe_ = 0;
  for (uint32_t i = 0; i < had; ++i) {
    if (old_tags[i]) {
      FindOrInsert(owner, old_keys[i], old_values[i]);
    }
  }
}

void RunMap::clear() {
  if (size_) {
    memset(tags_.data(), 0, tags_.size());
    size_ = 0;
    max_probe_ = 0;
  }
}

}  // namespace perfetto::trace_processor::core::exec
