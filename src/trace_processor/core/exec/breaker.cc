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

#include "src/trace_processor/core/exec/breaker.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

Breaker::~Breaker() = default;

void Breaker::Reset() {
  source_.Reset();
  Rewind();
  filled_ = false;
  status_ = base::OkStatus();
}

base::Status Breaker::Fill() {
  while (RowBatch* chunk = source_.Next()) {
    RETURN_IF_ERROR(Consume(*chunk));
  }
  RETURN_IF_ERROR(source_.status());
  return Finish();
}

RowBatch* Breaker::Next() {
  if (!filled_) {
    filled_ = true;
    status_ = Fill();
    if (!status_.ok()) {
      return nullptr;
    }
  }
  return Emit();
}

base::Status Breaker::status() const {
  return status_.ok() ? source_.status() : status_;
}

}  // namespace perfetto::trace_processor::core::exec
