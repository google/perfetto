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

#include "src/trace_processor/core/exec/context.h"

#include <cstring>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {

Context::Context() = default;

Context::~Context() {
  nulls_ = ColumnBuffer();
}

const ColumnBuffer& Context::NullBuffer() {
  if (PERFETTO_UNLIKELY(!nulls_)) {
    // Shared by columns of every type, so as wide as the widest.
    nulls_ = TakeBuffer(ColumnChunk::kMaxValueSize);
    ColumnChunk& chunk = nulls_.chunk();
    memset(chunk.Values<uint8_t>(), 0,
           kMaxBatchRows * ColumnChunk::kMaxValueSize);
    chunk.validity = BitVector::CreateWithSize(kMaxBatchRows, false);
  }
  return nulls_;
}

}  // namespace perfetto::trace_processor::core::exec
