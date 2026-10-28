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

#include "src/protovm/program.h"

#include <string>
#include <utility>

#include "perfetto/ext/base/span.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/protovm/vm_program.pbzero.h"
#include "src/protovm/error_handling.h"
#include "src/protovm/executor.h"

namespace perfetto {
namespace protovm {

StatusOr<Program> ProgramDecoder::Decode(protozero::ConstBytes proto_bytes) {
  ProgramDecoder decoder;
  protos::pbzero::VmProgram::Decoder proto_program(proto_bytes);
  if (proto_program.has_version()) {
    decoder.program_.version = proto_program.version();
  }

  CountResult counts =
      CountInstructionsAndPathComponents(proto_program.instructions());
  decoder.program_.instructions.reserve(counts.instructions);
  decoder.program_.path_components.reserve(counts.path_components);

  PROTOVM_ASSIGN_OR_RETURN(
      base::Span<const Instruction> root_instructions,
      decoder.DecodeInstructions(proto_program.instructions()));
  decoder.program_.root_instructions_size =
      static_cast<uint16_t>(root_instructions.size());
  return std::move(decoder.program_);
}

ProgramDecoder::CountResult ProgramDecoder::CountInstructionsAndPathComponents(
    protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it) {
  CountResult counts{};
  for (; proto_it; ++proto_it) {
    ++counts.instructions;
    protos::pbzero::VmInstruction::Decoder proto_instruction(*proto_it);
    if (proto_instruction.has_select()) {
      protos::pbzero::VmOpSelect::Decoder proto_select(
          proto_instruction.select());
      counts.path_components +=
          CountPathComponents(proto_select.relative_path());
    }
    CountResult nested_counts = CountInstructionsAndPathComponents(
        proto_instruction.nested_instructions());
    counts.instructions += nested_counts.instructions;
    counts.path_components += nested_counts.path_components;
  }
  return counts;
}

uint64_t ProgramDecoder::CountPathComponents(
    protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it) {
  uint64_t count = 0;
  while (proto_it) {
    protos::pbzero::VmOpSelect::PathComponent::Decoder proto_curr_component(
        *proto_it);
    ++proto_it;

    if (!proto_curr_component.is_repeated() && proto_it) {
      protos::pbzero::VmOpSelect::PathComponent::Decoder proto_next_component(
          *proto_it);
      if (proto_next_component.has_array_index() ||
          proto_next_component.has_map_key_field_id()) {
        ++proto_it;
      }
    }
    ++count;
  }
  return count;
}

StatusOr<base::Span<const Instruction>> ProgramDecoder::DecodeInstructions(
    protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it) {
  const uint16_t first_idx =
      static_cast<uint16_t>(program_.instructions.size());
  uint16_t count = 0;

  for (auto proto_cur = proto_it; proto_cur; ++proto_cur, ++count) {
    protos::pbzero::VmInstruction::Decoder proto_instruction(*proto_cur);
    auto instruction = DecodeInstruction(proto_instruction);
    PROTOVM_RETURN_IF_NOT_OK(instruction, "instruction[%d]",
                             static_cast<int>(count));
    if (program_.instructions.size() >= program_.instructions.capacity()) {
      PROTOVM_ABORT("Internal error: instructions capacity exceeded");
    }
    program_.instructions.push_back(*instruction);
  }

  uint16_t i = 0;
  for (auto proto_cur = proto_it; proto_cur; ++proto_cur, ++i) {
    protos::pbzero::VmInstruction::Decoder proto_instruction(*proto_cur);
    if (!proto_instruction.has_nested_instructions()) {
      continue;
    }
    auto nested_instructions =
        DecodeInstructions(proto_instruction.nested_instructions());
    PROTOVM_RETURN_IF_NOT_OK(nested_instructions, "instruction[%d]",
                             static_cast<int>(i));
    Instruction& instruction = program_.instructions[first_idx + i];
    instruction.common.nested_offset = static_cast<uint16_t>(
        nested_instructions->b - program_.instructions.data());
    instruction.common.nested_size =
        static_cast<uint16_t>(nested_instructions->size());
  }

  const Instruction* begin = program_.instructions.data() + first_idx;
  return base::Span<const Instruction>(begin, begin + count);
}

StatusOr<Instruction> ProgramDecoder::DecodeInstruction(
    const protos::pbzero::VmInstruction::Decoder& proto_instruction) {
  InstructionCommon common{};
  common.abort_level =
      proto_instruction.has_abort_level()
          ? static_cast<AbortLevel>(proto_instruction.abort_level())
          : AbortLevel::kSkipCurrentInstructionAndBreakOuter;

  if (proto_instruction.has_select()) {
    common.type = InstructionType::kSelect;
    protos::pbzero::VmOpSelect::Decoder proto_select(
        proto_instruction.select());
    CursorEnum selected = !proto_select.has_cursor()
                              ? CursorEnum::VM_CURSOR_SRC
                              : static_cast<CursorEnum>(proto_select.cursor());
    bool create_if_not_exist = proto_select.create_if_not_exist();

    if (selected == CursorEnum::VM_CURSOR_SRC && create_if_not_exist) {
      PROTOVM_ABORT(
          "incompatible params: src cursor (read only) + create_if_not_exist");
    }

    auto path_components =
        DecodePathComponents(proto_select.relative_path(), selected);
    PROTOVM_RETURN_IF_NOT_OK(path_components, "select");
    auto path_offset = static_cast<uint16_t>(path_components->b -
                                             program_.path_components.data());
    auto path_count = static_cast<uint16_t>(path_components->size());

    Instruction instruction{};
    instruction.select =
        InstructionSelect{common, static_cast<uint8_t>(selected),
                          create_if_not_exist, path_offset, path_count};
    return instruction;
  }

  if (proto_instruction.has_reg_load()) {
    common.type = InstructionType::kRegLoad;
    protos::pbzero::VmOpRegLoad::Decoder proto_reg_load(
        proto_instruction.reg_load());
    CursorEnum selected =
        !proto_reg_load.has_cursor()
            ? CursorEnum::VM_CURSOR_SRC
            : static_cast<CursorEnum>(proto_reg_load.cursor());
    uint8_t dst_register = static_cast<uint8_t>(proto_reg_load.dst_register());

    Instruction instruction{};
    instruction.reg_load = InstructionRegLoad{
        common, static_cast<uint8_t>(selected), dst_register};
    return instruction;
  }

  if (proto_instruction.has_del()) {
    common.type = InstructionType::kDel;
    Instruction instruction{};
    instruction.del = InstructionDel{common};
    return instruction;
  }

  if (proto_instruction.has_merge()) {
    common.type = InstructionType::kMerge;
    protos::pbzero::VmOpMerge::Decoder proto_merge(proto_instruction.merge());
    uint8_t flags = Executor::MergeFlags::kNone;
    if (proto_merge.skip_submessages()) {
      flags |= Executor::MergeFlags::kSkipSubmessages;
    }
    if (proto_merge.del_if_src_empty()) {
      flags |= Executor::MergeFlags::kDelIfSrcEmpty;
    }

    Instruction instruction{};
    instruction.merge = InstructionMerge{common, flags};
    return instruction;
  }

  if (proto_instruction.has_set()) {
    common.type = InstructionType::kSet;
    Instruction instruction{};
    instruction.set = InstructionSet{common};
    return instruction;
  }

  PROTOVM_ABORT("Unsupported instruction");
}

StatusOr<base::Span<const PathComponent>> ProgramDecoder::DecodePathComponents(
    protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it,
    CursorEnum selected_cursor) {
  const uint16_t first_idx =
      static_cast<uint16_t>(program_.path_components.size());
  uint16_t count = 0;

  while (proto_it) {
    protos::pbzero::VmOpSelect::PathComponent::Decoder proto_curr_component(
        *proto_it);
    ++proto_it;

    if (!proto_curr_component.has_field_id()) {
      PROTOVM_ABORT("Invalid path. Expected path component with field_id.");
    }

    PathComponent component{};
    if (proto_curr_component.is_repeated()) {
      if (selected_cursor != CursorEnum::VM_CURSOR_SRC &&
          selected_cursor != CursorEnum::VM_CURSOR_DST) {
        PROTOVM_ABORT(
            "Iteration over selected cursor (%d) is not supported. Should be "
            "either SRC or DST cursor.",
            static_cast<int>(selected_cursor));
      }
      component.repeated_for_each = PathComponentRepeatedForEach{
          PathComponentType::kRepeatedForEach, proto_curr_component.field_id()};
    } else if (proto_it) {
      protos::pbzero::VmOpSelect::PathComponent::Decoder proto_next_component(
          *proto_it);
      if (proto_next_component.has_array_index()) {
        ++proto_it;
        component.repeated_by_index =
            PathComponentRepeatedByIndex{PathComponentType::kRepeatedByIndex,
                                         proto_curr_component.field_id(),
                                         proto_next_component.array_index()};
      } else if (proto_next_component.has_map_key_field_id()) {
        ++proto_it;
        if (!proto_next_component.has_register_to_match()) {
          PROTOVM_ABORT(
              "enter mapped repeated field: expected field "
              "'register_to_match'");
        }
        component.repeated_by_key = PathComponentRepeatedByKey{
            PathComponentType::kRepeatedByKey,
            static_cast<uint8_t>(proto_next_component.register_to_match()),
            proto_curr_component.field_id(),
            proto_next_component.map_key_field_id()};
      } else {
        component.field_id = PathComponentFieldId{
            PathComponentType::kFieldId, proto_curr_component.field_id()};
      }
    } else {
      component.field_id = PathComponentFieldId{
          PathComponentType::kFieldId, proto_curr_component.field_id()};
    }

    if (program_.path_components.size() >=
        program_.path_components.capacity()) {
      PROTOVM_ABORT("Internal error: path_components capacity exceeded");
    }
    program_.path_components.push_back(component);
    ++count;
  }

  const PathComponent* begin = program_.path_components.data() + first_idx;
  return base::Span<const PathComponent>(begin, begin + count);
}

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
    case InstructionType::kSelect: {
      auto* proto_select = proto_instruction->set_select();
      proto_select->set_cursor(
          static_cast<protos::pbzero::VmCursorEnum>(instruction.select.cursor));
      EncodePathComponents(instruction.select.GetPathComponents(program),
                           proto_select);
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
