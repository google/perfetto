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

#include "src/trace_processor/core/exec/selection.h"

#include <cstdint>
#include <cstring>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

void Selection::Keep(Span<const uint32_t> positions) {
  auto count = static_cast<uint32_t>(positions.size());
  PERFETTO_DCHECK(count <= size_);
  if (count == size_) {
    // Increasing, they can only be every position.
    return;
  }
  if (count == 0 || positions.b[count - 1] == count - 1) {
    // Increasing and ending at count - 1, they are the first `count`: the
    // rows kept stay as they are, a prefix if they were one, so readers keep
    // their contiguous paths.
    size_ = count;
    return;
  }
  if (prefix_) {
    for (uint32_t i = 0; i < count; ++i) {
      indices_[i] = static_cast<uint16_t>(positions.b[i]);
    }
  } else {
    // Each index is written at or before where it is read from.
    for (uint32_t i = 0; i < count; ++i) {
      PERFETTO_DCHECK(positions.b[i] < size_ &&
                      (i == 0 || positions.b[i - 1] < positions.b[i]));
      indices_[i] = indices_[positions.b[i]];
    }
  }
  size_ = count;
  prefix_ = false;
}

void Selection::KeepRows(Span<const uint32_t> rows) {
  auto count = static_cast<uint32_t>(rows.size());
  PERFETTO_DCHECK(count <= size_);
  if (count == size_) {
    // Kept already, they can only be every row kept.
    return;
  }
  size_ = count;
  if (count == 0 || rows.b[count - 1] == count - 1) {
    // Increasing and ending at count - 1, they are the first `count`.
    prefix_ = true;
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    PERFETTO_DCHECK(rows.b[i] < rows_ && (i == 0 || rows.b[i - 1] < rows.b[i]));
    indices_[i] = static_cast<uint16_t>(rows.b[i]);
  }
  prefix_ = false;
}

void Selection::KeepRange(uint32_t first, uint32_t count) {
  PERFETTO_DCHECK(first + count <= size_);
  if (first != 0) {
    if (prefix_) {
      for (uint32_t i = 0; i < count; ++i) {
        indices_[i] = static_cast<uint16_t>(first + i);
      }
    } else {
      memmove(indices_, indices_ + first, count * sizeof(indices_[0]));
    }
    prefix_ = false;
  }
  size_ = count;
}

void Selection::CopyFrom(const Selection& other) {
  rows_ = other.rows_;
  size_ = other.size_;
  prefix_ = other.prefix_;
  if (!prefix_) {
    memcpy(indices_, other.indices_, size_ * sizeof(indices_[0]));
  }
}

}  // namespace perfetto::trace_processor::core::exec
