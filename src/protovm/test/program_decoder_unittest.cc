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

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/protovm/vm_program.pb.h"
#include "src/base/test/utils.h"
#include "src/protovm/test/sample_programs.h"
#include "src/protovm/test/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace protovm {
namespace test {

// The happy-path decoding of all instruction and path component types is
// already indirectly tested in VmTest and ParserTest.
// This test focuses on decoder internals and
// edge cases that are not observable through ParserTest or VmTest:
// - Optimizations and their boundaries (e.g. inlining
//    single-component paths vs. falling back to Span<PathComponent>).
// - Exact pre-allocation of instruction and path component vectors (`size() ==
//   capacity()`).
// - Decode-time validations of malformed instructions or out-of-bounds values.

TEST(ProgramDecoderTest, Select_InlinedPathComponent) {
  std::string bytes =
      SamplePrograms::Select_InlinedPathComponent().SerializeAsString();
  StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
  ASSERT_TRUE(program.IsOk());
  ASSERT_TRUE(program->path_components.empty());

  base::Span<const Instruction> instructions = program->GetRootInstructions();
  ASSERT_EQ(instructions.size(), 2u);
  ASSERT_EQ(instructions[0].type(), InstructionType::kSelectInlinedFieldId);
  ASSERT_EQ(instructions[1].type(),
            InstructionType::kSelectInlinedRepeatedForEach);
}

TEST(ProgramDecoderTest,
     Select_NotInlinedPathComponentWhenFieldIdExceedsUint16Max) {
  std::string bytes =
      SamplePrograms::
          Select_NotInlinedPathComponentWhenFieldIdExceedsUint16Max()
              .SerializeAsString();
  StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
  ASSERT_TRUE(program.IsOk());
  ASSERT_EQ(program->path_components.size(), 2u);

  base::Span<const Instruction> instructions = program->GetRootInstructions();
  ASSERT_EQ(instructions.size(), 2u);

  ASSERT_EQ(instructions[0].type(), InstructionType::kSelect);
  base::Span<const PathComponent> path0 =
      instructions[0].select.GetPathComponents(*program);
  ASSERT_EQ(path0.size(), 1u);
  ASSERT_EQ(path0[0].type(), PathComponentType::kFieldId);

  ASSERT_EQ(instructions[1].type(), InstructionType::kSelect);
  base::Span<const PathComponent> path1 =
      instructions[1].select.GetPathComponents(*program);
  ASSERT_EQ(path1.size(), 1u);
  ASSERT_EQ(path1[0].type(), PathComponentType::kRepeatedForEach);
}

TEST(ProgramDecoderTest, Select_FusedPathComponents) {
  std::string bytes =
      SamplePrograms::Select_FusedPathComponents().SerializeAsString();
  StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
  ASSERT_TRUE(program.IsOk());
  ASSERT_EQ(program->path_components.size(), 2u);

  base::Span<const Instruction> instructions = program->GetRootInstructions();
  ASSERT_EQ(instructions.size(), 1u);
  ASSERT_EQ(instructions[0].type(), InstructionType::kSelect);

  base::Span<const PathComponent> path =
      instructions[0].select.GetPathComponents(*program);
  ASSERT_EQ(path.size(), 2u);
  ASSERT_EQ(path[0].type(), PathComponentType::kRepeatedByIndex);
  ASSERT_EQ(path[1].type(), PathComponentType::kRepeatedByKey);
}

TEST(ProgramDecoderTest, RejectsTooManyPathComponents) {
  {
    // A single select instruction exceeding UINT8_MAX path components.
    perfetto::protos::VmProgram proto_program{};
    auto* select = proto_program.add_instructions()->mutable_select();
    for (int i = 0; i < 256; ++i) {
      select->add_relative_path()->set_field_id(1);
    }
    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsAbort());
  }
}

TEST(ProgramDecoderTest, RejectsTooManyInstructions) {
  perfetto::protos::VmProgram proto_program{};
  const uint32_t max_instructions = std::numeric_limits<
      decltype(InstructionCommon::nested_instructions_offset)>::max();
  for (uint32_t i = 0; i <= max_instructions; ++i) {
    proto_program.add_instructions()->mutable_del();
  }
  std::string bytes = proto_program.SerializeAsString();
  StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
  ASSERT_TRUE(program.IsAbort());
}

TEST(ProgramDecoderTest, RejectsTooLargeRegisterId) {
  {
    perfetto::protos::VmProgram proto_program{};
    proto_program.add_instructions()->mutable_reg_load()->set_dst_register(256);
    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsAbort());
  }
  {
    perfetto::protos::VmProgram proto_program{};
    auto* select = proto_program.add_instructions()->mutable_select();
    select->add_relative_path()->set_field_id(1);
    auto* map_key = select->add_relative_path();
    map_key->set_map_key_field_id(2);
    map_key->set_register_to_match(256);
    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsAbort());
  }
}

TEST(ProgramDecoderTest, RejectsSrcSelectWithCreateIfNotExist) {
  perfetto::protos::VmProgram proto_program{};
  auto* select = proto_program.add_instructions()->mutable_select();
  select->set_cursor(perfetto::protos::VM_CURSOR_SRC);
  select->set_create_if_not_exist(true);
  select->add_relative_path()->set_field_id(1);

  std::string bytes = proto_program.SerializeAsString();
  StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
  ASSERT_TRUE(program.IsAbort());
}

TEST(ProgramDecoderTest, RejectsInvalidAbortLevel) {
  for (int32_t invalid_abort_level : {-1, 0, 4, 100}) {
    protozero::HeapBuffered<perfetto::protos::pbzero::VmProgram> proto_program;
    auto* instr = proto_program->add_instructions();
    instr->set_del();
    instr->set_abort_level(
        static_cast<perfetto::protos::pbzero::VmInstruction::AbortLevel>(
            invalid_abort_level));

    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsAbort());
  }
}

TEST(ProgramDecoderTest, RejectsInvalidCursor) {
  // select instruction
  for (int32_t invalid_cursor : {-1, 0, 4, 100}) {
    protozero::HeapBuffered<perfetto::protos::pbzero::VmProgram> proto_program;
    auto* select = proto_program->add_instructions()->set_select();
    select->set_cursor(
        static_cast<perfetto::protos::pbzero::VmCursorEnum>(invalid_cursor));
    select->add_relative_path()->set_field_id(1);

    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsAbort());
  }
  // reg_load instruction
  for (int32_t invalid_cursor : {-1, 0, 4, 5}) {
    protozero::HeapBuffered<perfetto::protos::pbzero::VmProgram> proto_program;
    auto* reg_load = proto_program->add_instructions()->set_reg_load();
    reg_load->set_cursor(
        static_cast<perfetto::protos::pbzero::VmCursorEnum>(invalid_cursor));
    reg_load->set_dst_register(0);

    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsAbort());
  }
}

// Rejects VM_CURSOR_BOTH because not supported yet
TEST(ProgramDecoderTest, RejectsCursorBoth) {
  {
    protozero::HeapBuffered<perfetto::protos::pbzero::VmProgram> proto_program;
    auto* select = proto_program->add_instructions()->set_select();
    select->set_cursor(perfetto::protos::pbzero::VM_CURSOR_BOTH);
    select->add_relative_path()->set_field_id(1);

    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsAbort());
  }
  {
    protozero::HeapBuffered<perfetto::protos::pbzero::VmProgram> proto_program;
    auto* select = proto_program->add_instructions()->set_select();
    select->set_cursor(perfetto::protos::pbzero::VM_CURSOR_BOTH);
    auto* component = select->add_relative_path();
    component->set_field_id(1);
    component->set_is_repeated(true);

    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsAbort());
  }
  {
    protozero::HeapBuffered<perfetto::protos::pbzero::VmProgram> proto_program;
    auto* reg_load = proto_program->add_instructions()->set_reg_load();
    reg_load->set_cursor(perfetto::protos::pbzero::VM_CURSOR_BOTH);
    reg_load->set_dst_register(0);

    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsAbort());
  }
}

TEST(ProgramDecoderTest, RejectsTooDeepInstructionNesting) {
  {
    perfetto::protos::VmProgram proto_program{};
    auto* instr = proto_program.add_instructions();
    instr->mutable_del();
    for (uint32_t i = 0; i < ProgramDecoder::kMaxInstructionNestingDepth; ++i) {
      instr = instr->add_nested_instructions();
      instr->mutable_del();
    }
    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsOk());
  }
  {
    perfetto::protos::VmProgram proto_program{};
    auto* instr = proto_program.add_instructions();
    instr->mutable_del();
    for (uint32_t i = 0; i <= ProgramDecoder::kMaxInstructionNestingDepth;
         ++i) {
      instr = instr->add_nested_instructions();
      instr->mutable_del();
    }
    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsAbort());
  }
}

TEST(ProgramDecoderTest, RejectsInvalidPathComponents) {
  {
    perfetto::protos::VmProgram proto_program{};
    auto* select = proto_program.add_instructions()->mutable_select();
    select->add_relative_path();
    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsAbort());
  }
  {
    perfetto::protos::VmProgram proto_program{};
    auto* select = proto_program.add_instructions()->mutable_select();
    select->add_relative_path()->set_field_id(1);
    select->add_relative_path()->set_map_key_field_id(2);
    std::string bytes = proto_program.SerializeAsString();
    StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
    ASSERT_TRUE(program.IsAbort());
  }
}

TEST(ProgramDecoderTest, RejectsEmptyInstruction) {
  perfetto::protos::VmProgram proto_program{};
  proto_program.add_instructions();

  std::string bytes = proto_program.SerializeAsString();
  StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
  ASSERT_TRUE(program.IsAbort());
}

TEST(ProgramDecoderTest, MemoryUsage_MinimizesVectorCapacity) {
  perfetto::protos::VmProgram proto_program{};
  {
    auto* select = proto_program.add_instructions()->mutable_select();
    select->add_relative_path()->set_field_id(1);
    select->add_relative_path()->set_field_id(2);
  }
  {
    auto* select = proto_program.add_instructions()->mutable_select();
    select->add_relative_path()->set_field_id(3);
    select->add_relative_path()->set_field_id(4);
  }

  std::string bytes = proto_program.SerializeAsString();
  StatusOr<Program> program = ProgramDecoder::Decode(AsConstBytes(bytes));
  ASSERT_TRUE(program.IsOk());
  ASSERT_EQ(program->instructions.size(), 2u);
  ASSERT_EQ(program->instructions.capacity(), program->instructions.size());
  ASSERT_EQ(program->path_components.size(), 4u);
  ASSERT_EQ(program->path_components.capacity(),
            program->path_components.size());
}

TEST(ProgramDecoderTest, MemoryUsage_SurfaceFlinger) {
  std::string trace;
  ASSERT_TRUE(base::ReadFile(
      base::GetTestDataPath("test/data/protovm_surfaceflinger.pftrace"),
      &trace));
  protozero::ConstBytes program_bytes = ExtractProgram(trace);
  StatusOr<Program> program = ProgramDecoder::Decode(program_bytes);
  ASSERT_TRUE(program.IsOk());
  ASSERT_EQ(program->GetMemoryUsageBytes(), sizeof(void*) == 8 ? 1712u : 1688u);
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
