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

#include "src/trace_processor/core/exec/row_cursor.h"

#include "src/trace_processor/core/exec/operator.h"

namespace perfetto::trace_processor::core::exec {

RowCursor::RowCursor(const Source& source)
    : source_(source),
      state_(source.MakeState(context_)),
      may_be_in_place_(source.HasInPlace()) {}

RowCursor::RowCursor(const Source& source, const Pipelines& pipelines)
    : source_(source), may_be_in_place_(source.HasInPlace()) {
  context_.SetPipelines(&pipelines);
  state_ = source.MakeState(context_);
}

RowCursor::~RowCursor() = default;

bool RowCursor::Pull() {
  placed_ += size_;
  for (;;) {
    batch_ = source_.Next(scratch_, *state_);
    if (!batch_) {
      index_ = 0;
      size_ = 0;
      current_ = nullptr;
      return false;
    }
    exhausted_ = batch_->last();
    if (batch_->size()) {
      index_ = 0;
      size_ = batch_->size();
      if (PERFETTO_UNLIKELY(may_be_in_place_)) {
        in_place_ = source_.InPlace(*state_);
      }
      Locate();
      return true;
    }
    if (exhausted_) {
      index_ = 0;
      size_ = 0;
      current_ = nullptr;
      return false;
    }
  }
}

}  // namespace perfetto::trace_processor::core::exec
