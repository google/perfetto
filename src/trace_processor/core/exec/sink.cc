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

#include "src/trace_processor/core/exec/sink.h"

#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"

namespace perfetto::trace_processor::core::exec {

Sink::Sink(Source& source) : source_(source) {}

Sink::~Sink() = default;

bool Sink::Pull() {
  RowBatch* chunk = source_.Next();
  if (!chunk) {
    index_ = 0;
    size_ = 0;
    return false;
  }
  RowSelection selection = chunk->column(0).selection();
  rows_ = selection.is_range() ? nullptr : selection.data();
  base_ = selection.offset();
  index_ = 0;
  size_ = chunk->size();
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
