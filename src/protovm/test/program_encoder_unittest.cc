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

#include "perfetto/ext/base/file_utils.h"
#include "protos/perfetto/protovm/vm_program.pb.h"
#include "src/base/test/utils.h"
#include "src/protovm/program_decoder.h"
#include "src/protovm/test/sample_programs.h"
#include "src/protovm/test/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace protovm {
namespace test {

namespace {

void PopulateDefaultFields(perfetto::protos::VmInstruction* instruction) {
  if (!instruction->has_abort_level()) {
    instruction->set_abort_level(perfetto::protos::VmInstruction::
                                     SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER);
  }
  if (instruction->has_select()) {
    auto* select = instruction->mutable_select();
    if (!select->has_cursor()) {
      select->set_cursor(perfetto::protos::VM_CURSOR_SRC);
    }
    if (!select->has_create_if_not_exist()) {
      select->set_create_if_not_exist(false);
    }
  } else if (instruction->has_reg_load()) {
    auto* reg_load = instruction->mutable_reg_load();
    if (!reg_load->has_cursor()) {
      reg_load->set_cursor(perfetto::protos::VM_CURSOR_SRC);
    }
    if (!reg_load->has_dst_register()) {
      reg_load->set_dst_register(0);
    }
  } else if (instruction->has_merge()) {
    auto* merge = instruction->mutable_merge();
    if (!merge->has_skip_submessages()) {
      merge->set_skip_submessages(false);
    }
    if (!merge->has_del_if_src_empty()) {
      merge->set_del_if_src_empty(false);
    }
  }
  for (int i = 0; i < instruction->nested_instructions_size(); ++i) {
    PopulateDefaultFields(instruction->mutable_nested_instructions(i));
  }
}

perfetto::protos::VmProgram PopulateDefaultFields(
    perfetto::protos::VmProgram program) {
  if (!program.has_version()) {
    program.set_version(0);
  }
  for (int i = 0; i < program.instructions_size(); ++i) {
    PopulateDefaultFields(program.mutable_instructions(i));
  }
  return program;
}

void Check(const perfetto::protos::VmProgram& proto_program) {
  std::string original_bytes = proto_program.SerializeAsString();
  StatusOr<Program> decoded_program =
      ProgramDecoder::Decode(AsConstBytes(original_bytes));
  ASSERT_TRUE(decoded_program.IsOk());
  std::string reencoded_program = ProgramEncoder::Encode(*decoded_program);

  // ProgramEncoder uses protozero, which writes 4-byte redundant varints for
  // submessage lengths > 127 bytes, whereas libprotobuf writes canonical
  // varints. Re-serializing through VmProgram (proto2, which preserves explicit
  // field presence for default values) normalizes the length varints.
  perfetto::protos::VmProgram reencoded_proto;
  ASSERT_TRUE(reencoded_proto.ParseFromString(reencoded_program));

  std::string original_bytes_with_defaults =
      PopulateDefaultFields(proto_program).SerializeAsString();
  ASSERT_EQ(reencoded_proto.SerializeAsString(), original_bytes_with_defaults);
}

}  // namespace

TEST(ProgramEncoderTest, NoInstructions) {
  Check(SamplePrograms::NoInstructions());
}

TEST(ProgramEncoderTest, Select_AllCursorTypes) {
  Check(SamplePrograms::Select_AllCursorTypes());
}

TEST(ProgramEncoderTest, Select_InlinedPathComponent) {
  Check(SamplePrograms::Select_InlinedPathComponent());
}

TEST(ProgramEncoderTest,
     Select_NotInlinedPathComponentWhenFieldIdExceedsUint16Max) {
  Check(SamplePrograms::
            Select_NotInlinedPathComponentWhenFieldIdExceedsUint16Max());
}

TEST(ProgramEncoderTest, Select_FusedPathComponents) {
  Check(SamplePrograms::Select_FusedPathComponents());
}

TEST(ProgramEncoderTest, Select_AllFieldTypes) {
  Check(SamplePrograms::Select_AllFieldTypes());
}

TEST(ProgramEncoderTest, RegLoad) {
  Check(SamplePrograms::RegLoad());

  perfetto::protos::VmProgram proto_program{};
  auto* reg_load = proto_program.add_instructions()->mutable_reg_load();
  reg_load->set_cursor(perfetto::protos::VM_CURSOR_DST);
  reg_load->set_dst_register(5);
  Check(proto_program);
}

TEST(ProgramEncoderTest, Delete) {
  Check(SamplePrograms::Delete());
}

TEST(ProgramEncoderTest, Merge) {
  Check(SamplePrograms::Merge());
}

TEST(ProgramEncoderTest, Merge_DelIfSrcEmpty) {
  Check(SamplePrograms::Merge_DelIfSrcEmpty());
}

TEST(ProgramEncoderTest, Set) {
  Check(SamplePrograms::Set());
}

TEST(ProgramEncoderTest, AbortLevel_SKIP_CURRENT_INSTRUCTION) {
  Check(SamplePrograms::AbortLevel_SKIP_CURRENT_INSTRUCTION());
}

TEST(ProgramEncoderTest, AbortLevel_SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER) {
  Check(SamplePrograms::AbortLevel_SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER());
}

TEST(ProgramEncoderTest, AbortLevel_ABORT) {
  Check(SamplePrograms::AbortLevel_ABORT());
}

TEST(ProgramEncoderTest, SelectWithSinglePathComponent) {
  perfetto::protos::VmProgram proto_program{};

  // inlined path component
  {
    auto* instruction = proto_program.add_instructions();
    auto* select = instruction->mutable_select();
    select->set_cursor(perfetto::protos::VM_CURSOR_SRC);
    select->set_create_if_not_exist(false);
    select->add_relative_path()->set_field_id(10);
  }

  // inlined path component
  {
    auto* instruction = proto_program.add_instructions();
    auto* select = instruction->mutable_select();
    select->set_cursor(perfetto::protos::VM_CURSOR_DST);
    select->set_create_if_not_exist(true);
    auto* component = select->add_relative_path();
    component->set_field_id(20);
    component->set_is_repeated(true);
  }

  // non-inlined path component
  {
    auto* instruction = proto_program.add_instructions();
    auto* select = instruction->mutable_select();
    select->set_cursor(perfetto::protos::VM_CURSOR_SRC);
    select->set_create_if_not_exist(false);
    select->add_relative_path()->set_field_id(70000);
  }

  // non-inlined repeated path component
  {
    auto* instruction = proto_program.add_instructions();
    auto* select = instruction->mutable_select();
    select->set_cursor(perfetto::protos::VM_CURSOR_SRC);
    select->set_create_if_not_exist(false);
    auto* component = select->add_relative_path();
    component->set_field_id(70001);
    component->set_is_repeated(true);
  }

  Check(proto_program);
}

TEST(ProgramEncoderTest, NestedInstructions) {
  perfetto::protos::VmProgram proto_program{};
  auto* instruction = proto_program.add_instructions();
  instruction->mutable_set();

  {
    auto* nested_instruction = instruction->add_nested_instructions();
    nested_instruction->mutable_set();
  }
  {
    auto* nested_instruction = instruction->add_nested_instructions();
    nested_instruction->mutable_del();
  }

  Check(proto_program);
}

TEST(ProgramEncoderTest, SurfaceFlingerProgram) {
  std::string trace;
  ASSERT_TRUE(base::ReadFile(
      base::GetTestDataPath("test/data/protovm_surfaceflinger.pftrace"),
      &trace));
  protozero::ConstBytes program_bytes = ExtractBytes(trace);
  perfetto::protos::VmProgram proto_program;
  ASSERT_TRUE(proto_program.ParseFromArray(
      program_bytes.data, static_cast<int>(program_bytes.size)));
  Check(proto_program);
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
