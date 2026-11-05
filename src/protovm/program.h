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

// Used to access common fields of the path component. It is the common initial
// sequence of the PathComponent union.
struct PathComponentCommon {
  uint32_t field_id;
  PathComponentType type;
};

struct PathComponentFieldId {
  uint32_t field_id;
  PathComponentType type;
};

struct PathComponentRepeatedForEach {
  uint32_t field_id;
  PathComponentType type;
};

struct PathComponentRepeatedByIndex {
  uint32_t field_id;
  PathComponentType type;
  uint32_t array_index;
};

struct PathComponentRepeatedByKey {
  uint32_t field_id;
  PathComponentType type;
  uint8_t register_to_match;
  uint32_t map_key_field_id;
};

union PathComponent {
  PathComponentCommon common;
  PathComponentFieldId field_id;
  PathComponentRepeatedForEach repeated_for_each;
  PathComponentRepeatedByIndex repeated_by_index;
  PathComponentRepeatedByKey repeated_by_key;

  PERFETTO_ALWAYS_INLINE PathComponentType type() const { return common.type; }
};

#define PROTOVM_ASSERT_PATH_COMPONENT_LAYOUT(T)   \
  static_assert(std::is_standard_layout_v<T>);    \
  static_assert(std::is_trivially_copyable_v<T>); \
  static_assert(offsetof(T, field_id) == 0);      \
  static_assert(offsetof(T, type) == 4);

PROTOVM_ASSERT_PATH_COMPONENT_LAYOUT(PathComponentCommon)
PROTOVM_ASSERT_PATH_COMPONENT_LAYOUT(PathComponentFieldId)
PROTOVM_ASSERT_PATH_COMPONENT_LAYOUT(PathComponentRepeatedForEach)
PROTOVM_ASSERT_PATH_COMPONENT_LAYOUT(PathComponentRepeatedByIndex)
PROTOVM_ASSERT_PATH_COMPONENT_LAYOUT(PathComponentRepeatedByKey)
static_assert(sizeof(PathComponent) == 12);
static_assert(static_cast<uint8_t>(PathComponentType::kRepeatedByKey) <=
              std::numeric_limits<uint8_t>::max());

enum class InstructionType : uint8_t {
  kSelect = 0,
  kSelectInlinedFieldId = 1,
  kSelectInlinedRepeatedForEach = 2,
  kRegLoad = 3,
  kMerge = 4,
  kSet = 5,
  kDel = 6,
};

enum class AbortLevel : uint8_t {
  kSkipCurrentInstruction = 1,
  kSkipCurrentInstructionAndBreakOuter = 2,
  kAbort = 3,
};

enum class Cursor : uint8_t {
  kUnspecified = 0,
  kSrc = 1,
  kDst = 2,
  kBoth = 3,
};

union Instruction;

// Used to access common fields of the instruction. It is the common initial
// sequence of the Instruction union.
struct InstructionCommon {
  uint16_t nested_instructions_offset;
  uint16_t nested_instructions_size;
  InstructionType type : 3;
  AbortLevel abort_level : 2;

  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetNestedInstructions(
      const Program& program) const;
};

struct InstructionSelect {
  uint16_t nested_instructions_offset;
  uint16_t nested_instructions_size;
  InstructionType type : 3;
  AbortLevel abort_level : 2;
  Cursor cursor : 2;
  bool create_if_not_exist : 1;
  uint8_t path_components_size;
  union {
    uint16_t path_components_offset;
    // Optimization: avoid pointing to a Span<PathComponent> and inline the
    // field ID directly in the instruction. This is possible when the path has
    // one single component and the field ID fits in 16 bits. In practice a lot
    // of select instructions specify a single path component (recursive merges)
    // and fall into this optimization. With the SurfaceFlinger program this
    // optimization alone delivers:
    // - PathComponents instances reduced by 67%
    // - Decoded program memory footprint reduced by 36%
    // - CPU cycles reduced by 7%
    uint16_t inlined_field_id;
  };

  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetNestedInstructions(
      const Program& program) const;
  PERFETTO_ALWAYS_INLINE base::Span<const PathComponent> GetPathComponents(
      const Program& program) const;
};

struct InstructionRegLoad {
  uint16_t nested_instructions_offset;
  uint16_t nested_instructions_size;
  InstructionType type : 3;
  AbortLevel abort_level : 2;
  Cursor cursor;
  uint8_t dst_register;
};

struct InstructionMerge {
  uint16_t nested_instructions_offset;
  uint16_t nested_instructions_size;
  InstructionType type : 3;
  AbortLevel abort_level : 2;
  uint8_t merge_flags;
};

struct InstructionSet {
  uint16_t nested_instructions_offset;
  uint16_t nested_instructions_size;
  InstructionType type : 3;
  AbortLevel abort_level : 2;
};

struct InstructionDel {
  uint16_t nested_instructions_offset;
  uint16_t nested_instructions_size;
  InstructionType type : 3;
  AbortLevel abort_level : 2;
};

#define PROTOVM_ASSERT_INSTRUCTION_LAYOUT(T)                   \
  static_assert(std::is_standard_layout_v<T>);                 \
  static_assert(std::is_trivially_copyable_v<T>);              \
  static_assert(offsetof(T, nested_instructions_offset) == 0); \
  static_assert(offsetof(T, nested_instructions_size) == 2)

PROTOVM_ASSERT_INSTRUCTION_LAYOUT(InstructionCommon);
PROTOVM_ASSERT_INSTRUCTION_LAYOUT(InstructionSelect);
PROTOVM_ASSERT_INSTRUCTION_LAYOUT(InstructionRegLoad);
PROTOVM_ASSERT_INSTRUCTION_LAYOUT(InstructionMerge);
PROTOVM_ASSERT_INSTRUCTION_LAYOUT(InstructionSet);
PROTOVM_ASSERT_INSTRUCTION_LAYOUT(InstructionDel);
static_assert(static_cast<uint8_t>(InstructionType::kDel) < (1u << 3));
static_assert(static_cast<uint8_t>(AbortLevel::kAbort) < (1u << 2));
static_assert(static_cast<uint8_t>(Cursor::kBoth) < (1u << 2));

union Instruction {
  InstructionCommon common;
  InstructionSelect select;
  InstructionRegLoad reg_load;
  InstructionMerge merge;
  InstructionSet set;
  InstructionDel del;

  PERFETTO_ALWAYS_INLINE InstructionType type() const { return common.type; }
  PERFETTO_ALWAYS_INLINE AbortLevel abort_level() const {
    return common.abort_level;
  }
  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetNestedInstructions(
      const Program& program) const {
    return common.GetNestedInstructions(program);
  }

  StatusOr<void> SetNestedInstructions(uint64_t offset, uint64_t size);
};

static_assert(sizeof(Instruction) == 8);

struct Program {
  uint32_t version{0};
  uint16_t root_instructions_size{0};
  std::vector<Instruction> instructions;
  std::vector<PathComponent> path_components;

  PERFETTO_ALWAYS_INLINE base::Span<const Instruction> GetRootInstructions()
      const {
    PERFETTO_DCHECK(root_instructions_size <= instructions.size());
    return base::Span<const Instruction>(
        instructions.data(), instructions.data() + root_instructions_size);
  }

  uint64_t GetMemoryUsageBytes() const {
    return sizeof(Program) + instructions.capacity() * sizeof(Instruction) +
           path_components.capacity() * sizeof(PathComponent);
  }
};

inline PERFETTO_ALWAYS_INLINE base::Span<const Instruction>
InstructionCommon::GetNestedInstructions(const Program& program) const {
  PERFETTO_DCHECK(static_cast<size_t>(nested_instructions_offset) +
                      nested_instructions_size <=
                  program.instructions.size());
  const Instruction* begin =
      program.instructions.data() + nested_instructions_offset;
  return base::Span<const Instruction>(begin, begin + nested_instructions_size);
}

inline PERFETTO_ALWAYS_INLINE base::Span<const Instruction>
InstructionSelect::GetNestedInstructions(const Program& program) const {
  PERFETTO_DCHECK(static_cast<size_t>(nested_instructions_offset) +
                      nested_instructions_size <=
                  program.instructions.size());
  const Instruction* begin =
      program.instructions.data() + nested_instructions_offset;
  return base::Span<const Instruction>(begin, begin + nested_instructions_size);
}

inline PERFETTO_ALWAYS_INLINE base::Span<const PathComponent>
InstructionSelect::GetPathComponents(const Program& program) const {
  PERFETTO_DCHECK(type == InstructionType::kSelect);
  PERFETTO_DCHECK(static_cast<size_t>(path_components_offset) +
                      path_components_size <=
                  program.path_components.size());
  const PathComponent* begin =
      program.path_components.data() + path_components_offset;
  return base::Span<const PathComponent>(begin, begin + path_components_size);
}

}  // namespace protovm
}  // namespace perfetto

#endif  // SRC_PROTOVM_PROGRAM_H_
