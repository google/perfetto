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

#include "src/tracing/test/protovm_test_program.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto {

protos::gen::VmProgram CreateProtoVmProgram(uint32_t program_version) {
  auto add_copy_field_instructions =
      [](protos::gen::TestEvent::ProtoVmPatch::FieldNumbers src,
         protos::gen::TestEvent::ProtoVmIncrementalState::FieldNumbers dst,
         protos::gen::VmProgram* program) {
        // select src element
        auto* instr_src_select = program->add_instructions();
        auto* src_select = instr_src_select->mutable_select();

        src_select->add_relative_path()->set_field_id(
            protos::gen::TracePacket::kForTestingFieldNumber);

        src_select->add_relative_path()->set_field_id(
            protos::gen::TestEvent::kProtovmPatchFieldNumber);

        src_select->add_relative_path()->set_field_id(src);

        // select dst element
        auto* instr_dst_select = instr_src_select->add_nested_instructions();
        auto* dst_select = instr_dst_select->mutable_select();
        dst_select->set_cursor(protos::gen::VmCursorEnum::VM_CURSOR_DST);
        dst_select->set_create_if_not_exist(true);

        dst_select->add_relative_path()->set_field_id(
            protos::gen::TracePacket::kForTestingFieldNumber);
        dst_select->add_relative_path()->set_field_id(
            protos::gen::TestEvent::kProtovmIncrementalStateFieldNumber);
        dst_select->add_relative_path()->set_field_id(dst);

        // set dst = src
        auto* instr_set = instr_dst_select->add_nested_instructions();
        instr_set->mutable_set();
      };

  protos::gen::VmProgram program;
  program.set_version(program_version);
  add_copy_field_instructions(
      protos::gen::TestEvent::ProtoVmPatch::kStringToMergeFieldNumber,
      protos::gen::TestEvent::ProtoVmIncrementalState::kStringMergedFieldNumber,
      &program);
  add_copy_field_instructions(
      protos::gen::TestEvent::ProtoVmPatch::kIntToMergeFieldNumber,
      protos::gen::TestEvent::ProtoVmIncrementalState::kIntMergedFieldNumber,
      &program);

  return program;
}

}  // namespace perfetto
