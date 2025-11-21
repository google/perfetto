/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/protovm/vm.h"
#include "perfetto/protozero/field.h"
#include "src/protovm/error_handling.h"
#include "src/protovm/ro_cursor.h"
#include "src/protovm/rw_proto.h"

namespace perfetto {
namespace protovm {

Vm::Vm(std::string program, size_t memory_limit_bytes)
    : state_(std::in_place_type_t<ReadWriteState>{},
             std::move(program),
             memory_limit_bytes) {}

Vm::Vm(std::string incremental_state)
    : state_(std::in_place_type_t<ReadOnlyState>{},
             std::move(incremental_state)) {}

StatusOr<void> Vm::ApplyPatch(protozero::ConstBytes packet) {
  ReadWriteState* rw_state = std::get_if<ReadWriteState>(&state_);
  if (!rw_state) {
    return StatusOr<void>::Abort();
  }
  auto src = RoCursor(packet);
  auto dst = rw_state->incremental_state.GetRoot();
  return rw_state->parser.Run(src, dst);
}

std::string Vm::SerializeIncrementalState() const {
  if (const ReadOnlyState* state = std::get_if<ReadOnlyState>(&state_); state) {
    return state->serialized_incremental_state;
  }

  const ReadWriteState* state = std::get_if<ReadWriteState>(&state_);
  return state->incremental_state.SerializeAsString();
}

std::unique_ptr<Vm> Vm::CloneReadOnly() const {
  auto incremental_state = SerializeIncrementalState();
  return std::unique_ptr<Vm>(new Vm(std::move(incremental_state)));
}

}  // namespace protovm
}  // namespace perfetto
