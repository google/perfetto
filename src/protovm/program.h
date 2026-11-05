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

#include <cstdint>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/span.h"

#include "src/protovm/error_handling.h"

namespace perfetto {
namespace protovm {

struct Program;

enum class PathComponentType : uint8_t {
  kFieldId = 0,
  kRepeatedForEach = 1,
  kRepeatedByIndex = 2,
  kRepeatedByKey = 3,
};

struct PathComponent {
  uint32_t field_id;
  PathComponentType type;
  uint8_t register_to_match;  // kRepeatedByKey.
  union {
    uint32_t array_index;       // kRepeatedByIndex.
    uint32_t map_key_field_id;  // kRepeatedByKey.
  };
};
static_assert(sizeof(PathComponent) == 12);

enum class InstructionType : uint8_t {
  kSelect = 0,
  kSelectInlinedFieldId = 1,
  kSelectInlinedRepeatedForEach = 2,
  kRegLoad = 3,
  kMerge = 4,
  kSet = 5,
  kDel = 6,
};

// Keep aligned with VmInstruction::AbortLevel in
// protos/perfetto/protovm/vm_program.proto.
enum class AbortLevel : uint8_t {
  kSkipCurrentInstruction = 1,
  kSkipCurrentInstructionAndBreakOuter = 2,
  kAbort = 3,
};

enum class CursorSelection : uint8_t {
  kUnspecified = 0,
  kSrc = 1,
  kDst = 2,
};

struct Instruction {
  uint16_t nested_instructions_offset;
  uint16_t nested_instructions_size;
  // Keep all bit-fields on 1-byte unsigned types so MSVC packs them into one
  // byte alongside Clang and GCC.
  InstructionType type : 3;
  AbortLevel abort_level : 2;
  CursorSelection cursor : 2;    // kSelect*, kRegLoad.
  bool create_if_not_exist : 1;  // kSelect*.
  union {
    uint8_t path_components_size;  // kSelect.
    uint8_t dst_register;          // kRegLoad.
    uint8_t merge_flags;           // kMerge.
  };
  union {
    uint16_t path_components_offset;  // kSelect.

    // Optimization: avoid pointing to a Span<PathComponent> and inline the
    // field ID directly in the instruction. This is possible when the path has
    // one single component and the field ID fits in 16 bits. In practice a lot
    // of select instructions specify a single path component (recursive merges)
    // and fall into this optimization.
    uint16_t inlined_field_id;  // kSelectInlined*.
  };

  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetNestedInstructions(
      const Program& program) const;
  PERFETTO_ALWAYS_INLINE base::Span<const PathComponent> GetPathComponents(
      const Program& program) const;
  StatusOr<void> SetNestedInstructions(uint64_t offset, uint64_t size);
};
static_assert(sizeof(Instruction) == 8);

struct Program {
  uint16_t root_instructions_size{0};
  std::vector<Instruction> instructions;
  std::vector<PathComponent> path_components;

  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetRootInstructions()
      const {
    return base::MakeSpan(instructions).subspan(0, root_instructions_size);
  }

  uint64_t GetMemoryUsageBytes() const {
    return sizeof(Program) + instructions.capacity() * sizeof(Instruction) +
           path_components.capacity() * sizeof(PathComponent);
  }
};

inline PERFETTO_ALWAYS_INLINE base::Span<const Instruction>
Instruction::GetNestedInstructions(const Program& program) const {
  return base::MakeSpan(program.instructions)
      .subspan(nested_instructions_offset, nested_instructions_size);
}

inline PERFETTO_ALWAYS_INLINE base::Span<const PathComponent>
Instruction::GetPathComponents(const Program& program) const {
  PERFETTO_DCHECK(type == InstructionType::kSelect);
  return base::MakeSpan(program.path_components)
      .subspan(path_components_offset, path_components_size);
}

}  // namespace protovm
}  // namespace perfetto

#endif  // SRC_PROTOVM_PROGRAM_H_
