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

#include "protos/perfetto/protovm/vm_program.pb.h"
#include "src/protovm/test/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace protovm {
namespace test {

TEST(ProgramTest, DecodeAndEncode) {
  perfetto::protos::VmProgram proto_program{};
  proto_program.set_version(1);

  // All instruction types
  {
    {
      auto* instruction = proto_program.add_instructions();
      instruction->set_abort_level(
          perfetto::protos::VmInstruction::SKIP_CURRENT_INSTRUCTION);
      auto* select = instruction->mutable_select();
      select->set_cursor(perfetto::protos::VM_CURSOR_DST);
      select->set_create_if_not_exist(true);
    }
    {
      auto* instruction = proto_program.add_instructions();
      instruction->set_abort_level(
          perfetto::protos::VmInstruction::
              SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER);
      auto* reg_load = instruction->mutable_reg_load();
      reg_load->set_cursor(perfetto::protos::VM_CURSOR_SRC);
      reg_load->set_dst_register(1);
    }
    {
      auto* instruction = proto_program.add_instructions();
      instruction->set_abort_level(perfetto::protos::VmInstruction::ABORT);
      auto* merge = instruction->mutable_merge();
      merge->set_skip_submessages(true);
      merge->set_del_if_src_empty(false);
    }
    {
      auto* instruction = proto_program.add_instructions();
      instruction->set_abort_level(
          perfetto::protos::VmInstruction::
              SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER);
      instruction->mutable_set();
    }
    {
      auto* instruction = proto_program.add_instructions();
      instruction->set_abort_level(
          perfetto::protos::VmInstruction::
              SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER);
      instruction->mutable_del();
    }
  }

  // All path component types
  {
    auto* instruction = proto_program.add_instructions();
    instruction->set_abort_level(perfetto::protos::VmInstruction::
                                     SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER);
    auto* select = instruction->mutable_select();
    select->set_cursor(perfetto::protos::VM_CURSOR_SRC);
    select->set_create_if_not_exist(false);

    // field_id
    {
      auto* component = select->add_relative_path();
      component->set_field_id(1);
    }

    // repeated foreach
    {
      auto* component = select->add_relative_path();
      component->set_field_id(2);
      component->set_is_repeated(true);
    }

    // repeated by index
    {
      auto* component_field_id = select->add_relative_path();
      component_field_id->set_field_id(3);
      auto* component_index = select->add_relative_path();
      component_index->set_array_index(0);
    }

    // repeated by key
    {
      auto* component_field_id = select->add_relative_path();
      component_field_id->set_field_id(4);
      auto* component_map_key = select->add_relative_path();
      component_map_key->set_map_key_field_id(5);
      component_map_key->set_register_to_match(1);
    }
  }

  // Nested instructions
  {
    auto* instruction = proto_program.add_instructions();
    instruction->set_abort_level(perfetto::protos::VmInstruction::
                                     SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER);
    instruction->mutable_set();

    {
      auto* nested_instruction = instruction->add_nested_instructions();
      nested_instruction->set_abort_level(
          perfetto::protos::VmInstruction::
              SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER);
      nested_instruction->mutable_set();
    }
    {
      auto* nested_instruction = instruction->add_nested_instructions();
      nested_instruction->set_abort_level(
          perfetto::protos::VmInstruction::
              SKIP_CURRENT_INSTRUCTION_AND_BREAK_OUTER);
      nested_instruction->mutable_del();
    }
  }

  std::string bytes_original = proto_program.SerializeAsString();
  StatusOr<Program> program =
      ProgramDecoder::Decode(AsConstBytes(bytes_original));
  ASSERT_TRUE(program.IsOk());
  std::string bytes_reencoded = ProgramEncoder::Encode(*program);
  ASSERT_EQ(bytes_original, bytes_reencoded);
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
