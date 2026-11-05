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

#include "src/protovm/program_encoder.h"

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/protovm/executor.h"

namespace perfetto {
namespace protovm {

std::string ProgramEncoder::Encode(const Program& program) {
  protozero::HeapBuffered<protos::pbzero::VmProgram> proto_program;
  proto_program->set_version(program.version);
  for (const Instruction& instruction : program.GetRootInstructions()) {
    EncodeInstruction(program, instruction, proto_program->add_instructions());
  }
  return proto_program.SerializeAsString();
}

void ProgramEncoder::EncodeInstruction(
    const Program& program,
    const Instruction& instruction,
    protos::pbzero::VmInstruction* proto_instruction) {
  switch (instruction.type()) {
    case InstructionType::kSelect:
    case InstructionType::kSelectInlinedFieldId:
    case InstructionType::kSelectInlinedRepeatedForEach: {
      auto* proto_select = proto_instruction->set_select();
      proto_select->set_cursor(
          static_cast<protos::pbzero::VmCursorEnum>(instruction.select.cursor));
      if (instruction.type() == InstructionType::kSelectInlinedFieldId) {
        auto* proto_component = proto_select->add_relative_path();
        proto_component->set_field_id(instruction.select.inlined_field_id);
      } else if (instruction.type() ==
                 InstructionType::kSelectInlinedRepeatedForEach) {
        auto* proto_component = proto_select->add_relative_path();
        proto_component->set_field_id(instruction.select.inlined_field_id);
        proto_component->set_is_repeated(true);
      } else {
        EncodePathComponents(instruction.select.GetPathComponents(program),
                             proto_select);
      }
      proto_select->set_create_if_not_exist(
          instruction.select.create_if_not_exist);
      break;
    }
    case InstructionType::kRegLoad: {
      auto* proto_reg_load = proto_instruction->set_reg_load();
      proto_reg_load->set_cursor(static_cast<protos::pbzero::VmCursorEnum>(
          instruction.reg_load.cursor));
      proto_reg_load->set_dst_register(instruction.reg_load.dst_register);
      break;
    }
    case InstructionType::kMerge: {
      auto* proto_merge = proto_instruction->set_merge();
      proto_merge->set_skip_submessages(
          (instruction.merge.merge_flags &
           Executor::MergeFlags::kSkipSubmessages) != 0);
      proto_merge->set_del_if_src_empty(
          (instruction.merge.merge_flags &
           Executor::MergeFlags::kDelIfSrcEmpty) != 0);
      break;
    }
    case InstructionType::kSet: {
      proto_instruction->set_set();
      break;
    }
    case InstructionType::kDel: {
      proto_instruction->set_del();
      break;
    }
  }

  proto_instruction->set_abort_level(
      static_cast<protos::pbzero::VmInstruction::AbortLevel>(
          instruction.abort_level()));

  for (const Instruction& nested : instruction.GetNestedInstructions(program)) {
    EncodeInstruction(program, nested,
                      proto_instruction->add_nested_instructions());
  }
}

void ProgramEncoder::EncodePathComponents(
    base::Span<const PathComponent> path_components,
    protos::pbzero::VmOpSelect* proto_select) {
  for (const PathComponent& component : path_components) {
    switch (component.type()) {
      case PathComponentType::kFieldId: {
        auto* proto_component = proto_select->add_relative_path();
        proto_component->set_field_id(component.field_id.field_id);
        break;
      }
      case PathComponentType::kRepeatedForEach: {
        auto* proto_component = proto_select->add_relative_path();
        proto_component->set_field_id(component.repeated_for_each.field_id);
        proto_component->set_is_repeated(true);
        break;
      }
      case PathComponentType::kRepeatedByIndex: {
        auto* proto_field = proto_select->add_relative_path();
        proto_field->set_field_id(component.repeated_by_index.field_id);
        auto* proto_index = proto_select->add_relative_path();
        proto_index->set_array_index(component.repeated_by_index.array_index);
        break;
      }
      case PathComponentType::kRepeatedByKey: {
        auto* proto_field = proto_select->add_relative_path();
        proto_field->set_field_id(component.repeated_by_key.field_id);
        auto* proto_key = proto_select->add_relative_path();
        proto_key->set_map_key_field_id(
            component.repeated_by_key.map_key_field_id);
        proto_key->set_register_to_match(
            component.repeated_by_key.register_to_match);
        break;
      }
    }
  }
}

}  // namespace protovm
}  // namespace perfetto
