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

#include <memory>
#include <string>
#include <unordered_map>

#include "test/gtest_and_gmock.h"

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/common/descriptor.pbzero.h"
#include "src/protovm/compiler/android_extension.descriptor.h"
#include "src/protovm/compiler/compiler.h"
#include "src/protovm/compiler/trace.descriptor.h"
#include "src/protovm/compiler/vm_program.descriptor.h"
#include "src/protozero/text_to_proto/text_to_proto.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/protozero_to_text.h"

namespace perfetto::protovm {
namespace {

using perfetto::trace_processor::DescriptorPool;
using namespace perfetto::trace_processor::protozero_to_text;

class CompilerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pool_ = std::make_unique<DescriptorPool>();
    auto status =
        pool_->AddFromFileDescriptorSet(perfetto::kVmProgramDescriptor.data(),
                                        perfetto::kVmProgramDescriptor.size());
    EXPECT_TRUE(status.ok());
  }

  static std::string GetCombinedDescriptors() {
    return std::string(
               reinterpret_cast<const char*>(perfetto::kTraceDescriptor.data()),
               perfetto::kTraceDescriptor.size()) +
           std::string(reinterpret_cast<const char*>(
                           perfetto::kAndroidExtensionDescriptor.data()),
                       perfetto::kAndroidExtensionDescriptor.size());
  }

  void CheckCompilation(std::string_view config_textproto,
                        std::string_view expected_instructions_textproto) {
    Compiler compiler;
    base::StatusOr<std::string> actual_instructions_binary =
        compiler.Compile(config_textproto, GetCombinedDescriptors());
    EXPECT_TRUE(actual_instructions_binary.ok())
        << actual_instructions_binary.status().message();

    std::string actual_instructions_textproto = ProtozeroToText(
        *pool_, ".perfetto.protos.VmProgram",
        protozero::ConstBytes{reinterpret_cast<const uint8_t*>(
                                  actual_instructions_binary->data()),
                              actual_instructions_binary->size()});

    EXPECT_EQ(actual_instructions_textproto,
              NormalizeProgramTextProto(expected_instructions_textproto));
  }

 private:
  std::string NormalizeProgramTextProto(std::string_view textproto) {
    auto status_or_binary = protozero::TextToProto(
        perfetto::kVmProgramDescriptor.data(),
        perfetto::kVmProgramDescriptor.size(), ".perfetto.protos.VmProgram",
        "program.textproto", textproto);
    EXPECT_TRUE(status_or_binary.ok()) << status_or_binary.status().message();
    return ProtozeroToText(*pool_, ".perfetto.protos.VmProgram",
                           protozero::ConstBytes{status_or_binary->data(),
                                                 status_or_binary->size()});
  }

  std::unique_ptr<DescriptorPool> pool_;
};

TEST_F(CompilerTest, Set) {
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      set {
        src: "for_testing"
        src: "protovm_patch"
        src: "string_to_merge"
        dst: "for_testing"
        dst: "protovm_incremental_state"
        dst: "string_merged"
      }
      abort_level: ABORT
    })";

  std::string expected_instructions = R"(
    instructions {
      abort_level: ABORT
      select {
        relative_path {
          field_id: 900
        }
        relative_path {
          field_id: 6
        }
        relative_path {
          field_id: 1
        }
      }
      nested_instructions {
        select {
          cursor: VM_CURSOR_DST
          create_if_not_exist: true
          relative_path {
            field_id: 900
          }
          relative_path {
            field_id: 7
          }
          relative_path {
            field_id: 1
          }
        }
        nested_instructions {
          set {
          }
        }
      }
    })";

  CheckCompilation(config, expected_instructions);
}

TEST_F(CompilerTest, DelIfSrcPresent) {
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      del {
        src: "for_testing"
        src: "protovm_patch"
        src: "single_message"
        dst: "for_testing"
        dst: "protovm_incremental_state"
        if_src_present: true
      }
      abort_level: ABORT
    })";

  std::string expected_instructions = R"(
  instructions {
    abort_level: ABORT
    select {
      relative_path {
        field_id: 900
      }
      relative_path {
        field_id: 6
      }
      relative_path {
        field_id: 3
      }
    }
    nested_instructions {
      select {
        cursor: VM_CURSOR_DST
        relative_path {
          field_id: 900
        }
        relative_path {
          field_id: 7
        }
      }
      nested_instructions {
        del {}
      }
    }
  })";

  CheckCompilation(config, expected_instructions);
}

TEST_F(CompilerTest, DelByKey) {
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      del {
        src: "for_testing"
        src: "protovm_patch"
        src: "delete_message_ids"
        dst: "for_testing"
        dst: "protovm_incremental_state"
        dst: "messages"
        dst_key_field: "id"
      }
      abort_level: ABORT
    })";

  std::string expected_instructions = R"(
    instructions {
      abort_level: ABORT
      select {
        relative_path {
          field_id: 900
        }
        relative_path {
          field_id: 6
        }
        relative_path {
          field_id: 5
          is_repeated: true
        }
      }
      nested_instructions {
        reg_load {
          dst_register: 0
        }
      }
      nested_instructions {
        select {
          cursor: VM_CURSOR_DST
          relative_path {
            field_id: 900
          }
          relative_path {
            field_id: 7
          }
          relative_path {
            field_id: 4
          }
          relative_path {
            map_key_field_id: 1
            register_to_match: 0
          }
        }
        nested_instructions {
          del {
          }
        }
      }
    })";

  CheckCompilation(config, expected_instructions);
}

TEST_F(CompilerTest, Merge) {
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      merge {
        src: "for_testing"
        src: "protovm_patch"
        src: "single_message"
        dst: "for_testing"
        dst: "protovm_incremental_state"
        dst: "single_message"
      }
      abort_level: ABORT
    })";

  std::string expected_instructions = R"(
    instructions {
      abort_level: ABORT
      select {
        relative_path {
          field_id: 900
        }
        relative_path {
          field_id: 6
        }
        relative_path {
          field_id: 3
        }
      }
      nested_instructions {
        select {
          cursor: VM_CURSOR_DST
          create_if_not_exist: true
          relative_path {
            field_id: 900
          }
          relative_path {
            field_id: 7
          }
          relative_path {
            field_id: 3
          }
        }
        nested_instructions {
          merge {
            del_if_src_empty: true
          }
        }
      }
    })";

  CheckCompilation(config, expected_instructions);
}

TEST_F(CompilerTest, MergeRecursive) {
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      merge {
        src: "for_testing"
        src: "protovm_patch"
        src: "single_message"
        dst: "for_testing"
        dst: "protovm_incremental_state"
        dst: "single_message"
        recursive: true
      }
      abort_level: ABORT
    })";

  std::string expected_instructions = R"(
    instructions {
      abort_level: ABORT
      select {
        relative_path {
          field_id: 900
        }
        relative_path {
          field_id: 6
        }
        relative_path {
          field_id: 3
        }
      }
      nested_instructions {
        select {
          cursor: VM_CURSOR_DST
          create_if_not_exist: true
          relative_path {
            field_id: 900
          }
          relative_path {
            field_id: 7
          }
          relative_path {
            field_id: 3
          }
        }
        nested_instructions {
          abort_level: SKIP_CURRENT_INSTRUCTION
          select {
            relative_path {
              field_id: 2
            }
          }
          nested_instructions {
            select {
              cursor: VM_CURSOR_DST
              create_if_not_exist: true
              relative_path {
                field_id: 2
              }
            }
            nested_instructions {
              merge {
                del_if_src_empty: true
                skip_submessages: true
              }
            }
          }
        }
        nested_instructions {
          merge {
            del_if_src_empty: true
            skip_submessages: true
          }
        }
      }
    })";

  CheckCompilation(config, expected_instructions);
}

TEST_F(CompilerTest, MergeByKey) {
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      merge {
        src: "for_testing"
        src: "protovm_patch"
        src: "messages"
        dst: "for_testing"
        dst: "protovm_incremental_state"
        dst: "messages"
        key_field: "id"
      }
      abort_level: ABORT
    })";

  std::string expected_instructions = R"(
    instructions {
      abort_level: ABORT
      select {
        relative_path {
          field_id: 900
        }
        relative_path {
          field_id: 6
        }
        relative_path {
          field_id: 4
          is_repeated: true
        }
      }
      nested_instructions {
        abort_level: ABORT
        select {
          relative_path {
            field_id: 1
          }
        }
        nested_instructions {
          reg_load {
            dst_register: 0
          }
        }
      }
      nested_instructions {
        select {
          cursor: VM_CURSOR_DST
          create_if_not_exist: true
          relative_path {
            field_id: 900
          }
          relative_path {
            field_id: 7
          }
          relative_path {
            field_id: 4
          }
          relative_path {
            map_key_field_id: 1
            register_to_match: 0
          }
        }
        nested_instructions {
          merge {
            del_if_src_empty: true
          }
        }
      }
    })";

  CheckCompilation(config, expected_instructions);
}

TEST_F(CompilerTest, EnterScope) {
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      enter_scope {
        src: "for_testing"
        src: "protovm_patch"
        dst: "for_testing"
        dst: "protovm_incremental_state"
        commands {
          set {}
        }
      }
      abort_level: ABORT
    })";

  std::string expected_instructions = R"(
    instructions {
      abort_level: ABORT
      select {
        relative_path {
          field_id: 900
        }
        relative_path {
          field_id: 6
        }
      }
      nested_instructions {
        select {
          cursor: VM_CURSOR_DST
          create_if_not_exist: true
          relative_path {
            field_id: 900
          }
          relative_path {
            field_id: 7
          }
        }
        nested_instructions {
          set {
          }
        }
      }
    })";

  CheckCompilation(config, expected_instructions);
}

TEST_F(CompilerTest, AbortLevelTranslation) {
  std::unordered_map<std::string, std::string>
      command_to_instruction_abort_level{
          {"", ""},
          {"abort_level: SKIP_CURRENT_COMMAND",
           "abort_level: SKIP_CURRENT_INSTRUCTION"},
          {"abort_level: SKIP_CURRENT_COMMAND_AND_BREAK_OUTER", ""},
          {"abort_level: ABORT", "abort_level: ABORT"},
      };

  for (auto [command_abort, instruction_abort] :
       command_to_instruction_abort_level) {
    std::string config = R"(
      root_message: "perfetto.protos.TracePacket"
      commands {
        set {
          src: "for_testing"
          dst: "for_testing"
        }
        )" + command_abort +
                         R"(
      })";

    std::string expected_instructions = R"(
      instructions {
        )" + instruction_abort + R"(
        select {
          relative_path {
            field_id: 900
          }
        }
        nested_instructions {
          select {
            cursor: VM_CURSOR_DST
            create_if_not_exist: true
            relative_path {
              field_id: 900
            }
          }
          nested_instructions {
            set {
            }
          }
        }
      })";

    CheckCompilation(config, expected_instructions);
  }
}

TEST_F(CompilerTest, ErrorInvalidFieldName) {
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      set {
        src: "for_testing"
        src: "protovm_patch"
        src: "single_message"
        dst: "for_testing"
        dst: "protovm_incremental_state"
        dst: "invalid_field_name"
      }
    })";

  auto status_or = Compiler{}.Compile(config, GetCombinedDescriptors());
  EXPECT_FALSE(status_or.ok());
  EXPECT_THAT(
      status_or.status().message(),
      ::testing::HasSubstr("Failed to lookup field name: invalid_field_name"));
}

TEST_F(CompilerTest, EmptyPath) {
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      set {}
    }
    commands {
      set {
        src: "for_testing"
      }
    }
    commands {
      set {
        dst: "for_testing"
      }
    }
  )";

  std::string expected_instructions = R"(
    instructions {
      set {}
    }
    instructions {
      select {
        relative_path {
          field_id: 900
        }
      }
      nested_instructions {
        set {}
      }
    }
    instructions {
      select {
        cursor: VM_CURSOR_DST
        create_if_not_exist: true
        relative_path {
          field_id: 900
        }
      }
      nested_instructions {
        set {}
      }
    })";

  CheckCompilation(config, expected_instructions);
}

TEST_F(CompilerTest, CanResolveWinscopeExtensionsProtos) {
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      set {
        src: "for_testing"
        src: "protovm_patch"
        dst: "winscope_extensions"
        dst: "windowmanager"
        dst: "window_manager_service"
      }
    })";

  std::string expected_instructions = R"(
    instructions {
      select {
        relative_path {
          field_id: 900
        }
        relative_path {
          field_id: 6
        }
      }
      nested_instructions {
        select {
          cursor: VM_CURSOR_DST
          create_if_not_exist: true
          relative_path {
            field_id: 112
          }
          relative_path {
            field_id: 6
          }
          relative_path {
            field_id: 3
          }
        }
        nested_instructions {
          set {
          }
        }
      }
    })";

  CheckCompilation(config, expected_instructions);
}

TEST_F(CompilerTest, ErrorScalarFieldInPathMiddle) {
  // "string_to_merge" is a string: the path cannot continue past it. The
  // compiler must report an error, not crash.
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      set {
        src: "for_testing"
        src: "protovm_patch"
        src: "string_to_merge"
        src: "bogus"
        dst: "for_testing"
      }
    })";

  auto status_or = Compiler{}.Compile(config, GetCombinedDescriptors());
  EXPECT_FALSE(status_or.ok());
  EXPECT_THAT(status_or.status().message(),
              ::testing::HasSubstr("the parent field is not a message"));
}

TEST_F(CompilerTest, ErrorRecursiveMergeTypeMismatch) {
  // src is a ProtoVmMessage, dst is a ProtoVmIncrementalState: a recursive
  // merge between different message types is meaningless. The compiler must
  // report an error, not crash.
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      merge {
        src: "for_testing"
        src: "protovm_patch"
        src: "single_message"
        dst: "for_testing"
        dst: "protovm_incremental_state"
        recursive: true
      }
    })";

  auto status_or = Compiler{}.Compile(config, GetCombinedDescriptors());
  EXPECT_FALSE(status_or.ok());
  EXPECT_THAT(
      status_or.status().message(),
      ::testing::HasSubstr(
          "Recursive merge requires src and dst to be messages of the same"));
}

TEST_F(CompilerTest, ErrorRecursiveMergeOnScalarField) {
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      merge {
        src: "for_testing"
        src: "protovm_patch"
        src: "string_to_merge"
        dst: "for_testing"
        dst: "protovm_incremental_state"
        dst: "string_merged"
        recursive: true
      }
    })";

  auto status_or = Compiler{}.Compile(config, GetCombinedDescriptors());
  EXPECT_FALSE(status_or.ok());
  EXPECT_THAT(
      status_or.status().message(),
      ::testing::HasSubstr(
          "Recursive merge requires src and dst to be messages of the same"));
}

TEST_F(CompilerTest, ErrorKeyFieldNotNumericScalar) {
  // "submessage" is a message: it can never match a numeric register at
  // runtime, so the compiler must reject it as a key field.
  std::string config = R"(
    root_message: "perfetto.protos.TracePacket"
    commands {
      merge {
        src: "for_testing"
        src: "protovm_patch"
        src: "messages"
        dst: "for_testing"
        dst: "protovm_incremental_state"
        dst: "messages"
        key_field: "submessage"
      }
    })";

  auto status_or = Compiler{}.Compile(config, GetCombinedDescriptors());
  EXPECT_FALSE(status_or.ok());
  EXPECT_THAT(
      status_or.status().message(),
      ::testing::HasSubstr(
          "Key field 'submessage' must be a non-repeated numeric field"));
}

TEST_F(CompilerTest, ErrorRecursiveMergeCyclicMessage) {
  // A message type that contains itself via a non-repeated field would make
  // a recursive merge recurse forever. The compiler must report an error, not
  // crash.
  protozero::HeapBuffered<protos::pbzero::FileDescriptorSet> descriptor_set;
  auto* file = descriptor_set->add_file();
  file->set_name("cyclic.proto");
  file->set_package("cyclictest");
  auto* message = file->add_message_type();
  message->set_name("Cyclic");
  auto* field = message->add_field();
  field->set_name("child");
  field->set_number(1);
  field->set_label(protos::pbzero::FieldDescriptorProto::LABEL_OPTIONAL);
  field->set_type(protos::pbzero::FieldDescriptorProto::TYPE_MESSAGE);
  field->set_type_name(".cyclictest.Cyclic");

  std::string config = R"(
    root_message: "cyclictest.Cyclic"
    commands {
      merge {
        recursive: true
      }
    })";

  auto status_or =
      Compiler{}.Compile(config, descriptor_set.SerializeAsString());
  EXPECT_FALSE(status_or.ok());
  EXPECT_THAT(status_or.status().message(),
              ::testing::HasSubstr("cyclic message types are not supported"));
}

}  // namespace
}  // namespace perfetto::protovm
