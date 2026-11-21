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

#include "src/protovm/program_decoder.h"

#include "protos/perfetto/protovm/vm_program.pbzero.h"

#include "src/protovm/executor.h"

namespace perfetto {
namespace protovm {

namespace {

class ProgramDecoder {
 public:
  static StatusOr<Program> Decode(protozero::ConstBytes proto_bytes);

 private:
  struct CountResult {
    uint64_t instructions{0};
    uint64_t path_components{0};
  };

  static StatusOr<CountResult> CountInstructionsAndPathComponents(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it,
      uint32_t depth = 0);
  static uint64_t CountPathComponents(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it);
  static bool CanInlinePathIntoInstruction(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> path_it);

  StatusOr<base::Span<const Instruction>> DecodeInstructions(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it);
  StatusOr<Instruction> DecodeInstruction(
      const protos::pbzero::VmInstruction::Decoder& proto_instruction);
  StatusOr<Instruction> DecodeSelect(
      const protos::pbzero::VmOpSelect::Decoder& proto_select,
      AbortLevel abort_level);
  StatusOr<Instruction> DecodeRegLoad(
      const protos::pbzero::VmOpRegLoad::Decoder& proto_reg_load,
      AbortLevel abort_level);
  StatusOr<Instruction> DecodeMerge(
      const protos::pbzero::VmOpMerge::Decoder& proto_merge,
      AbortLevel abort_level);
  StatusOr<Instruction> DecodeSet(AbortLevel abort_level);
  StatusOr<Instruction> DecodeDel(AbortLevel abort_level);
  StatusOr<base::Span<const PathComponent>> DecodePathComponents(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it,
      CursorSelection selected_cursor);

  Program program_;
};

StatusOr<Program> ProgramDecoder::Decode(protozero::ConstBytes proto_bytes) {
  ProgramDecoder decoder;
  protos::pbzero::VmProgram::Decoder proto_program(proto_bytes);

  PROTOVM_ASSIGN_OR_RETURN(
      CountResult counts,
      CountInstructionsAndPathComponents(proto_program.instructions()));
  decoder.program_.instructions.reserve(counts.instructions);
  decoder.program_.path_components.reserve(counts.path_components);

  PROTOVM_ASSIGN_OR_RETURN(
      base::Span<const Instruction> root_instructions,
      decoder.DecodeInstructions(proto_program.instructions()));
  PROTOVM_CHECKED_CAST(
      decoder.program_.root_instructions_size, root_instructions.size(),
      "Too many root instructions (%zu)", root_instructions.size());
  return std::move(decoder.program_);
}

StatusOr<ProgramDecoder::CountResult>
ProgramDecoder::CountInstructionsAndPathComponents(
    protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it,
    uint32_t depth) {
  // Each recursive frame of CountInstructionsAndPathComponents and
  // DecodeInstructions holds a protozero::TypedProtoDecoder with >1KB of
  // inlined on stack storage. So let's enforce a recursion limit to avoid
  // untrusted input to cause stack overflows.
  if (depth >= kMaxInstructionNestingDepth) {
    PROTOVM_ABORT("Instruction nesting depth exceeds limit (%u)",
                  kMaxInstructionNestingDepth);
  }
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
    if (proto_instruction.has_nested_instructions()) {
      PROTOVM_ASSIGN_OR_RETURN(
          CountResult nested_counts,
          CountInstructionsAndPathComponents(
              proto_instruction.nested_instructions(), depth + 1));
      counts.instructions += nested_counts.instructions;
      counts.path_components += nested_counts.path_components;
    }
  }
  return counts;
}

uint64_t ProgramDecoder::CountPathComponents(
    protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it) {
  if (!proto_it) {
    return 0;
  }
  if (CanInlinePathIntoInstruction(proto_it)) {
    return 0;
  }

  uint64_t count = 0;
  while (proto_it) {
    protos::pbzero::VmOpSelect::PathComponent::Decoder proto_curr_component(
        *proto_it);
    ++proto_it;

    if (!proto_curr_component.is_repeated() && proto_it) {
      protos::pbzero::VmOpSelect::PathComponent::Decoder proto_next_component(
          *proto_it);
      // Do not count "index" or "map key" proto components, because in the
      // decoded format they are fused into the previous path component.
      if (proto_next_component.has_array_index() ||
          proto_next_component.has_map_key_field_id()) {
        ++proto_it;
      }
    }
    ++count;
  }
  return count;
}

bool ProgramDecoder::CanInlinePathIntoInstruction(
    protozero::RepeatedFieldIterator<protozero::ConstBytes> path_it) {
  if (!path_it) {
    return false;
  }
  protos::pbzero::VmOpSelect::PathComponent::Decoder first(*path_it);
  auto next_it = path_it;
  ++next_it;
  return !next_it && first.has_field_id() &&
         first.field_id() <=
             std::numeric_limits<
                 decltype(InstructionSelect::inlined_field_id)>::max();
}

StatusOr<base::Span<const Instruction>> ProgramDecoder::DecodeInstructions(
    protozero::RepeatedFieldIterator<protozero::ConstBytes>
        proto_instructions) {
  const uint64_t offset = program_.instructions.size();
  uint64_t curr_idx = offset;

  // First pass: decode instructions at current level.
  for (auto proto_it = proto_instructions; proto_it; ++proto_it, ++curr_idx) {
    protos::pbzero::VmInstruction::Decoder proto_instruction(*proto_it);
    auto instruction = DecodeInstruction(proto_instruction);
    PROTOVM_RETURN_IF_NOT_OK(instruction, "instruction[%" PRIu64 "]",
                             curr_idx - offset);
    if (program_.instructions.size() >= program_.instructions.capacity()) {
      PROTOVM_ABORT(
          "Internal error: instructions have wrong pre-allocated capacity");
    }
    program_.instructions.push_back(*instruction);
  }

  // Second pass: decode nested instructions, if needed.
  curr_idx = offset;
  for (auto proto_it = proto_instructions; proto_it; ++proto_it, ++curr_idx) {
    protos::pbzero::VmInstruction::Decoder proto_instruction(*proto_it);
    if (!proto_instruction.has_nested_instructions()) {
      continue;
    }
    auto nested_instructions =
        DecodeInstructions(proto_instruction.nested_instructions());
    PROTOVM_RETURN_IF_NOT_OK(nested_instructions, "instruction[%" PRIu64 "]",
                             curr_idx - offset);
    Instruction& instruction = program_.instructions[curr_idx];
    auto nested_instructions_offset = static_cast<uint64_t>(
        nested_instructions->begin() - program_.instructions.data());
    auto nested_instructions_size = nested_instructions->size();
    auto status = instruction.SetNestedInstructions(nested_instructions_offset,
                                                    nested_instructions_size);
    PROTOVM_RETURN_IF_NOT_OK(status);
  }

  const Instruction* begin = program_.instructions.data() + offset;
  const Instruction* end = program_.instructions.data() + curr_idx;
  return base::Span<const Instruction>(begin, end);
}

StatusOr<Instruction> ProgramDecoder::DecodeInstruction(
    const protos::pbzero::VmInstruction::Decoder& proto_instruction) {
  AbortLevel abort_level = AbortLevel::kSkipCurrentInstructionAndBreakOuter;
  if (proto_instruction.has_abort_level()) {
    PROTOVM_CHECKED_CAST_RANGE(abort_level, proto_instruction.abort_level(),
                               AbortLevel::kSkipCurrentInstruction,
                               AbortLevel::kAbort, "Invalid abort_level (%d)",
                               proto_instruction.abort_level());
  }

  if (proto_instruction.has_select()) {
    return DecodeSelect(
        protos::pbzero::VmOpSelect::Decoder(proto_instruction.select()),
        abort_level);
  }
  if (proto_instruction.has_reg_load()) {
    return DecodeRegLoad(
        protos::pbzero::VmOpRegLoad::Decoder(proto_instruction.reg_load()),
        abort_level);
  }
  if (proto_instruction.has_merge()) {
    return DecodeMerge(
        protos::pbzero::VmOpMerge::Decoder(proto_instruction.merge()),
        abort_level);
  }
  if (proto_instruction.has_set()) {
    return DecodeSet(abort_level);
  }
  if (proto_instruction.has_del()) {
    return DecodeDel(abort_level);
  }

  PROTOVM_ABORT("Unsupported instruction");
}

StatusOr<Instruction> ProgramDecoder::DecodeSelect(
    const protos::pbzero::VmOpSelect::Decoder& proto_select,
    AbortLevel abort_level) {
  CursorSelection selected = CursorSelection::kSrc;
  if (proto_select.has_cursor()) {
    PROTOVM_CHECKED_CAST_RANGE(selected, proto_select.cursor(),
                               CursorSelection::kSrc, CursorSelection::kDst,
                               "Invalid cursor (%d)", proto_select.cursor());
  }

  if (selected != CursorSelection::kDst && proto_select.create_if_not_exist()) {
    PROTOVM_ABORT(
        "incompatible params: src cursor (read only) + create_if_not_exist");
  }

  auto path_it = proto_select.relative_path();
  if (CanInlinePathIntoInstruction(path_it)) {
    protos::pbzero::VmOpSelect::PathComponent::Decoder first(*path_it);
    InstructionSelect select{};
    select.type = first.is_repeated()
                      ? InstructionType::kSelectInlinedRepeatedForEach
                      : InstructionType::kSelectInlinedFieldId;
    select.abort_level = abort_level;
    select.cursor = selected;
    select.create_if_not_exist = proto_select.create_if_not_exist();
    select.inlined_field_id = static_cast<uint16_t>(first.field_id());

    Instruction instruction{};
    instruction.select = select;
    return instruction;
  }

  auto path_components =
      DecodePathComponents(proto_select.relative_path(), selected);
  PROTOVM_RETURN_IF_NOT_OK(path_components, "select");

  decltype(InstructionSelect::path_components_offset) path_components_offset =
      0;
  PROTOVM_CHECKED_CAST(
      path_components_offset,
      static_cast<size_t>(path_components->begin() -
                          program_.path_components.data()),
      "Select path_components_offset (%zu) doesn't fit in 16 bits",
      static_cast<size_t>(path_components->begin() -
                          program_.path_components.data()));

  decltype(InstructionSelect::path_components_size) path_components_size = 0;
  PROTOVM_CHECKED_CAST(path_components_size, path_components->size(),
                       "Too many path components in select (%zu)",
                       path_components->size());

  InstructionSelect select{};
  select.type = InstructionType::kSelect;
  select.abort_level = abort_level;
  select.cursor = selected;
  select.create_if_not_exist = proto_select.create_if_not_exist();
  select.path_components_size = path_components_size;
  select.path_components_offset = path_components_offset;

  Instruction instruction{};
  instruction.select = select;
  return instruction;
}

StatusOr<Instruction> ProgramDecoder::DecodeRegLoad(
    const protos::pbzero::VmOpRegLoad::Decoder& proto_reg_load,
    AbortLevel abort_level) {
  CursorSelection selected = CursorSelection::kSrc;
  if (proto_reg_load.has_cursor()) {
    PROTOVM_CHECKED_CAST_RANGE(selected, proto_reg_load.cursor(),
                               CursorSelection::kSrc, CursorSelection::kDst,
                               "Invalid cursor (%d)", proto_reg_load.cursor());
  }
  if (!proto_reg_load.has_dst_register()) {
    PROTOVM_ABORT("reg_load doesn't have dst_register");
  }
  decltype(InstructionRegLoad::dst_register) dst_register = 0;
  PROTOVM_CHECKED_CAST(dst_register, proto_reg_load.dst_register(),
                       "Invalid dst_register (%u)",
                       proto_reg_load.dst_register());

  InstructionRegLoad reg_load{};
  reg_load.type = InstructionType::kRegLoad;
  reg_load.abort_level = abort_level;
  reg_load.cursor = selected;
  reg_load.dst_register = dst_register;

  Instruction instruction{};
  instruction.reg_load = reg_load;
  return instruction;
}

StatusOr<Instruction> ProgramDecoder::DecodeMerge(
    const protos::pbzero::VmOpMerge::Decoder& proto_merge,
    AbortLevel abort_level) {
  uint8_t flags = Executor::MergeFlags::kNone;
  if (proto_merge.skip_submessages()) {
    flags |= Executor::MergeFlags::kSkipSubmessages;
  }
  if (proto_merge.del_if_src_empty()) {
    flags |= Executor::MergeFlags::kDelIfSrcEmpty;
  }

  InstructionMerge merge{};
  merge.type = InstructionType::kMerge;
  merge.abort_level = abort_level;
  merge.merge_flags = flags;

  Instruction instruction{};
  instruction.merge = merge;
  return instruction;
}

StatusOr<Instruction> ProgramDecoder::DecodeSet(AbortLevel abort_level) {
  InstructionSet set{};
  set.type = InstructionType::kSet;
  set.abort_level = abort_level;

  Instruction instruction{};
  instruction.set = set;
  return instruction;
}

StatusOr<Instruction> ProgramDecoder::DecodeDel(AbortLevel abort_level) {
  InstructionDel del{};
  del.type = InstructionType::kDel;
  del.abort_level = abort_level;

  Instruction instruction{};
  instruction.del = del;
  return instruction;
}

StatusOr<base::Span<const PathComponent>> ProgramDecoder::DecodePathComponents(
    protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it,
    CursorSelection selected_cursor) {
  const size_t first_idx = program_.path_components.size();
  size_t count = 0;

  while (proto_it) {
    protos::pbzero::VmOpSelect::PathComponent::Decoder proto_curr_component(
        *proto_it);
    ++proto_it;
    std::optional<protos::pbzero::VmOpSelect::PathComponent::Decoder>
        proto_next_component;
    if (proto_it) {
      proto_next_component.emplace(*proto_it);
    }

    if (!proto_curr_component.has_field_id()) {
      PROTOVM_ABORT("Path component doesn't have field_id");
    }
    const uint32_t field_id = proto_curr_component.field_id();

    PathComponent component{};
    if (proto_curr_component.is_repeated()) {
      if (selected_cursor != CursorSelection::kSrc &&
          selected_cursor != CursorSelection::kDst) {
        PROTOVM_ABORT(
            "Iteration over selected cursor (%d) is not supported. Should be "
            "either SRC or DST cursor.",
            static_cast<int>(selected_cursor));
      }
      PathComponentRepeatedForEach repeated_for_each{};
      repeated_for_each.field_id = field_id;
      repeated_for_each.type = PathComponentType::kRepeatedForEach;
      component.repeated_for_each = repeated_for_each;
    } else if (proto_next_component &&
               proto_next_component->has_array_index()) {
      ++proto_it;
      PathComponentRepeatedByIndex repeated_by_index{};
      repeated_by_index.field_id = field_id;
      repeated_by_index.type = PathComponentType::kRepeatedByIndex;
      repeated_by_index.array_index = proto_next_component->array_index();
      component.repeated_by_index = repeated_by_index;
    } else if (proto_next_component &&
               proto_next_component->has_map_key_field_id()) {
      ++proto_it;
      if (!proto_next_component->has_register_to_match()) {
        PROTOVM_ABORT(
            "enter mapped repeated field: expected field "
            "'register_to_match'");
      }
      decltype(PathComponentRepeatedByKey::register_to_match)
          register_to_match = 0;
      PROTOVM_CHECKED_CAST(register_to_match,
                           proto_next_component->register_to_match(),
                           "Invalid register_to_match (%u)",
                           proto_next_component->register_to_match());
      PathComponentRepeatedByKey repeated_by_key{};
      repeated_by_key.field_id = field_id;
      repeated_by_key.type = PathComponentType::kRepeatedByKey;
      repeated_by_key.register_to_match = register_to_match;
      repeated_by_key.map_key_field_id =
          proto_next_component->map_key_field_id();
      component.repeated_by_key = repeated_by_key;
    } else {
      PathComponentFieldId field_id_component{};
      field_id_component.field_id = field_id;
      field_id_component.type = PathComponentType::kFieldId;
      component.field_id = field_id_component;
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

}  // namespace

StatusOr<Program> DecodeProgram(protozero::ConstBytes proto_bytes) {
  return ProgramDecoder::Decode(proto_bytes);
}

}  // namespace protovm
}  // namespace perfetto
