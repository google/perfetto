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

#include "src/trace_processor/core/exec/operator.h"

#include <cstring>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/run_held.h"

namespace perfetto::trace_processor::core::exec {

HeldState::~HeldState() = default;

void HeldState::EndHeldRuns() {
  const Registry& registry = *held_;
  for (const HeldValue& value : registry.values) {
    switch (value.size) {
      case 1:
        memcpy(value.where, &value.initial, 1);
        break;
      case 4:
        memcpy(value.where, &value.initial, 4);
        break;
      default:
        memcpy(value.where, &value.initial, 8);
        break;
    }
  }
  for (base::Status* status : registry.statuses) {
    if (!status->ok()) {
      *status = base::OkStatus();
    }
  }
  for (const HeldObject& held : registry.held) {
    held.end_run(held.where);
  }
}

void HeldState::AddHeld(void* where, void (*end_run)(void*)) {
  registry().held.push_back({where, end_run});
}

// Defined out of line so that each class's vtable is emitted in one object
// file.
RowBatch* Source::Next(RowBatch& scratch, OperatorState& state) const {
  return GetData(scratch, state) ? &scratch : nullptr;
}
OperatorState* Transform::NewState(Context& context) const {
  return context.Make<EmptyState>();
}
OperatorState* Operator::NewState(Context& context) const {
  return context.Make<EmptyState>();
}
base::Status Source::status(const OperatorState&) const {
  return base::OkStatus();
}
base::Status Transform::status(const OperatorState&) const {
  return base::OkStatus();
}
base::Status Operator::status(const OperatorState&) const {
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::core::exec
