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

#ifndef SRC_PROTOVM_PROGRAM_H_
#define SRC_PROTOVM_PROGRAM_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"
#include "protos/perfetto/protovm/vm_program.pbzero.h"

#include "src/protovm/error_handling.h"
#include "src/protovm/executor.h"

namespace perfetto {
namespace protovm {

struct Program;

enum class PathComponentType : uint8_t {
  kFieldId = 0,
  kRepeatedForEach = 1,
  kRepeatedByIndex = 2,
  kRepeatedByKey = 3,
};

struct PathComponentCommon {
  PathComponentType type;
};

struct PathComponentFieldId {
  PathComponentType type;
  uint32_t field_id;
};

struct PathComponentRepeatedForEach {
  PathComponentType type;
  uint32_t field_id;
};

struct PathComponentRepeatedByIndex {
  PathComponentType type;
  uint32_t field_id;
  uint32_t array_index;
};

struct PathComponentRepeatedByKey {
  PathComponentType type;
  uint8_t register_to_match;
  uint32_t field_id;
  uint32_t map_key_field_id;
};

union PathComponent {
  PathComponentCommon
      common;  // Used to access the type of the path component, which is part
               // of the union's common initial sequence.
  PathComponentFieldId field_id;
  PathComponentRepeatedForEach repeated_for_each;
  PathComponentRepeatedByIndex repeated_by_index;
  PathComponentRepeatedByKey repeated_by_key;

  PERFETTO_ALWAYS_INLINE PathComponentType type() const noexcept {
    return common.type;
  }
};

#define PROTOVM_ASSERT_PATH_COMPONENT_LAYOUT(T)   \
  static_assert(std::is_standard_layout_v<T>);    \
  static_assert(std::is_trivially_copyable_v<T>); \
  static_assert(offsetof(T, type) == 0);

PROTOVM_ASSERT_PATH_COMPONENT_LAYOUT(PathComponentFieldId)
PROTOVM_ASSERT_PATH_COMPONENT_LAYOUT(PathComponentRepeatedForEach)
PROTOVM_ASSERT_PATH_COMPONENT_LAYOUT(PathComponentRepeatedByIndex)
PROTOVM_ASSERT_PATH_COMPONENT_LAYOUT(PathComponentRepeatedByKey)
static_assert(sizeof(PathComponent) == 12);

enum class InstructionType : uint8_t {
  kSelect = 0,
  kRegLoad = 1,
  kMerge = 2,
  kSet = 3,
  kDel = 4,
};

enum class AbortLevel : uint8_t {
  kSkipCurrentInstruction = 1,
  kSkipCurrentInstructionAndBreakOuter = 2,
  kAbort = 3,
};

union Instruction;

struct InstructionCommon {
  InstructionType type;
  AbortLevel abort_level;
  uint16_t nested_offset;
  uint16_t nested_size;

  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetNestedInstructions(
      const Program& program) const noexcept;
};

struct InstructionSelect {
  InstructionCommon common;
  uint8_t cursor;
  bool create_if_not_exist;
  uint16_t path_offset;
  uint16_t path_count;

  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetNestedInstructions(
      const Program& program) const noexcept {
    return common.GetNestedInstructions(program);
  }
  PERFETTO_ALWAYS_INLINE base::Span<const PathComponent> GetPathComponents(
      const Program& program) const noexcept;
};

struct InstructionRegLoad {
  InstructionCommon common;
  uint8_t cursor;
  uint8_t dst_register;

  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetNestedInstructions(
      const Program& program) const noexcept {
    return common.GetNestedInstructions(program);
  }
};

struct InstructionMerge {
  InstructionCommon common;
  uint8_t merge_flags;

  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetNestedInstructions(
      const Program& program) const noexcept {
    return common.GetNestedInstructions(program);
  }
};

struct InstructionSet {
  InstructionCommon common;

  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetNestedInstructions(
      const Program& program) const noexcept {
    return common.GetNestedInstructions(program);
  }
};

struct InstructionDel {
  InstructionCommon common;

  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetNestedInstructions(
      const Program& program) const noexcept {
    return common.GetNestedInstructions(program);
  }
};

union Instruction {
  InstructionCommon
      common;  // Used to access the type of the instruction, which is part of
               // the union's common initial sequence.
  InstructionSelect select;
  InstructionRegLoad reg_load;
  InstructionMerge merge;
  InstructionSet set;
  InstructionDel del;

  PERFETTO_ALWAYS_INLINE InstructionType type() const noexcept {
    return common.type;
  }
  PERFETTO_ALWAYS_INLINE AbortLevel abort_level() const noexcept {
    return common.abort_level;
  }
  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetNestedInstructions(
      const Program& program) const noexcept {
    return common.GetNestedInstructions(program);
  }
};

#define PROTOVM_ASSERT_INSTRUCTION_LAYOUT(T)      \
  static_assert(std::is_standard_layout_v<T>);    \
  static_assert(std::is_trivially_copyable_v<T>); \
  static_assert(offsetof(T, common) == 0)

PROTOVM_ASSERT_INSTRUCTION_LAYOUT(InstructionSelect);
PROTOVM_ASSERT_INSTRUCTION_LAYOUT(InstructionRegLoad);
PROTOVM_ASSERT_INSTRUCTION_LAYOUT(InstructionMerge);
PROTOVM_ASSERT_INSTRUCTION_LAYOUT(InstructionSet);
PROTOVM_ASSERT_INSTRUCTION_LAYOUT(InstructionDel);
static_assert(sizeof(Instruction) == 12);

struct Program {
  uint32_t version{0};
  std::vector<Instruction> instructions;
  std::vector<PathComponent> path_components;
  uint16_t root_instructions_size{0};

  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetRootInstructions()
      const noexcept {
    return base::Span<const Instruction>(
        instructions.data(), instructions.data() + root_instructions_size);
  }

  uint64_t GetMemoryUsageBytes() const {
    return sizeof(Program) + instructions.capacity() * sizeof(Instruction) +
           path_components.capacity() * sizeof(PathComponent);
  }
};

inline PERFETTO_ALWAYS_INLINE base::Span<const Instruction>
InstructionCommon::GetNestedInstructions(
    const Program& program) const noexcept {
  const Instruction* begin = program.instructions.data() + nested_offset;
  return base::Span<const Instruction>(begin, begin + nested_size);
}

inline PERFETTO_ALWAYS_INLINE base::Span<const PathComponent>
InstructionSelect::GetPathComponents(const Program& program) const noexcept {
  const PathComponent* begin = program.path_components.data() + path_offset;
  return base::Span<const PathComponent>(begin, begin + path_count);
}

class ProgramDecoder {
 public:
  static StatusOr<Program> Decode(protozero::ConstBytes proto_bytes);

 private:
  struct CountResult {
    uint64_t instructions{0};
    uint64_t path_components{0};
  };

  static CountResult CountInstructionsAndPathComponents(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it);
  static uint64_t CountPathComponents(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it);

  StatusOr<base::Span<const Instruction>> DecodeInstructions(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it);
  StatusOr<Instruction> DecodeInstruction(
      const protos::pbzero::VmInstruction::Decoder& proto_instruction);
  StatusOr<base::Span<const PathComponent>> DecodePathComponents(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it,
      CursorEnum selected_cursor);

  Program program_;
};

class ProgramEncoder {
 public:
  static std::string Encode(const Program& program);

 private:
  static void EncodeInstruction(
      const Program& program,
      const Instruction& instruction,
      protos::pbzero::VmInstruction* proto_instruction);
  static void EncodePathComponents(
      base::Span<const PathComponent> path_components,
      protos::pbzero::VmOpSelect* proto_select);
};

}  // namespace protovm
}  // namespace perfetto

#endif  // SRC_PROTOVM_PROGRAM_H_
