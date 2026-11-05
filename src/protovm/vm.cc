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

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/protovm/program_decoder.h"

namespace perfetto {
namespace protovm {

base::StatusOr<std::unique_ptr<Vm>> Vm::Create(
    protozero::ConstBytes proto_program,
    size_t memory_limit_bytes,
    protozero::ConstBytes initial_incremental_state) {
  StatusOr<Program> decoded_program = ProgramDecoder::Decode(proto_program);
  if (!decoded_program.IsOk()) {
    PERFETTO_DCHECK(decoded_program.IsAbort());
    if (decoded_program.IsAbort()) {
      return base::ErrStatus(
          "Failed to decode ProtoVM program. Stacktrace:\n%s",
          base::Join(decoded_program.stacktrace(), "\n").c_str());
    }
    return base::ErrStatus("Failed to decode ProtoVM program");
  }
  auto vm = std::unique_ptr<Vm>(
      new Vm(proto_program, std::move(*decoded_program), memory_limit_bytes,
             initial_incremental_state));
  if (!vm) {
    return base::ErrStatus("Failed to allocate ProtoVM");
  }
  return std::move(vm);
}

Vm::Vm(protozero::ConstBytes proto_program,
       Program decoded_program,
       size_t memory_limit_bytes,
       protozero::ConstBytes initial_incremental_state)
    : proto_program_(proto_program.ToStdString()),
      decoded_program_(std::move(decoded_program)),
      state_(std::in_place_type_t<ReadWriteState>{},
             &decoded_program_,
             memory_limit_bytes,
             initial_incremental_state) {}

Vm::Vm(const Vm& other)
    : proto_program_(other.proto_program_),
      decoded_program_(other.decoded_program_),
      state_(std::in_place_type_t<ReadOnlyState>{},
             other.SerializeIncrementalStateAsString()) {}

StatusOr<void> Vm::ApplyPatch(protozero::ConstBytes packet) {
  ReadWriteState* rw_state = std::get_if<ReadWriteState>(&state_);
  if (!rw_state) {
    return StatusOr<void>::Abort();
  }
  auto src = RoCursor(packet);
  auto dst = rw_state->incremental_state.GetRoot();
  return rw_state->parser.Run(src, dst);
}

void Vm::SerializeIncrementalState(protozero::Message* proto) const {
  if (const ReadOnlyState* state = std::get_if<ReadOnlyState>(&state_); state) {
    proto->AppendRawProtoBytes(state->serialized_incremental_state.data(),
                               state->serialized_incremental_state.size());
    return;
  }

  const ReadWriteState* state = std::get_if<ReadWriteState>(&state_);
  state->incremental_state.Serialize(proto);
}

std::string Vm::SerializeIncrementalStateAsString() const {
  protozero::HeapBuffered<protozero::Message> proto;
  SerializeIncrementalState(proto.get());
  return proto.SerializeAsString();
}

const std::string& Vm::SerializeProgram() const {
  return proto_program_;
}

std::unique_ptr<Vm> Vm::CloneReadOnly() const {
  return std::unique_ptr<Vm>(new Vm(*this));
}

uint64_t Vm::GetMemoryUsageBytes() const {
  uint64_t program_bytes =
      proto_program_.size() + decoded_program_.GetMemoryUsageBytes();
  if (const ReadOnlyState* state = std::get_if<ReadOnlyState>(&state_); state) {
    return program_bytes + state->serialized_incremental_state.size();
  }

  const ReadWriteState* state = std::get_if<ReadWriteState>(&state_);
  return program_bytes + state->allocator.GetMemoryUsageBytes();
}

}  // namespace protovm
}  // namespace perfetto
