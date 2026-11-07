/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/protovm/test/sample_packets.h"
#include "test/gtest_and_gmock.h"

#include "src/protovm/program_decoder.h"
#include "src/protovm/test/protos/incremental_trace.pb.h"
#include "src/protovm/test/sample_programs.h"
#include "src/protovm/test/utils.h"
#include "src/protovm/vm.h"

namespace perfetto {
namespace protovm {
namespace test {

class VmTest : public ::testing::Test {
 protected:
  static constexpr size_t MEMORY_LIMIT_BYTES = 10 * 1024 * 1024;

  std::unique_ptr<Vm> CreateVm(
      const perfetto::protos::VmProgram& proto_program,
      std::optional<std::string> initial_incremental_state =
          std::nullopt) const {
    std::string serialized_program = proto_program.SerializeAsString();
    protozero::ConstBytes state_bytes =
        initial_incremental_state ? AsConstBytes(*initial_incremental_state)
                                  : protozero::ConstBytes{nullptr, 0};
    auto vm = Vm::Create(AsConstBytes(serialized_program), MEMORY_LIMIT_BYTES,
                         state_bytes);
    PERFETTO_CHECK(vm.ok());
    return std::move(*vm);
  }

  std::string InitialIncrementalState() const {
    protos::TraceEntry state{};
    auto* element0 = state.add_elements();
    element0->set_id(0);
    element0->set_value(10);
    auto* element1 = state.add_elements();
    element1->set_id(1);
    element1->set_value(11);
    return state.SerializeAsString();
  }

  std::string SerializeIncrementalStateAsString(const Vm& vm) const {
    protozero::HeapBuffered<protozero::Message> proto;
    vm.SerializeIncrementalState(proto.get());
    return proto.SerializeAsString();
  }
};

TEST_F(VmTest, NoPatch) {
  auto vm = CreateVm(SamplePrograms::IncrementalTraceInstructions());
  ASSERT_TRUE(SerializeIncrementalStateAsString(*vm).empty());
}

TEST_F(VmTest, InvalidProgramAbortsOnCreation) {
  perfetto::protos::VmProgram invalid_program{};
  invalid_program.add_instructions();  // Instruction with no operation set
  auto serialized = invalid_program.SerializeAsString();
  auto vm = Vm::Create(AsConstBytes(serialized), MEMORY_LIMIT_BYTES);
  ASSERT_FALSE(vm.ok());
}

TEST_F(VmTest, ConstructionWithInitialIncrementalState) {
  auto vm = CreateVm(SamplePrograms::IncrementalTraceInstructions(),
                     InitialIncrementalState());

  protos::TraceEntry state{};
  state.ParseFromString(SerializeIncrementalStateAsString(*vm));
  ASSERT_EQ(state.elements_size(), 2);
  ASSERT_EQ(state.elements(0).id(), 0);
  ASSERT_EQ(state.elements(0).value(), 10);
  ASSERT_EQ(state.elements(1).id(), 1);
  ASSERT_EQ(state.elements(1).value(), 11);
}

TEST_F(VmTest, ApplyPatch_DelOperation) {
  auto vm = CreateVm(SamplePrograms::IncrementalTraceInstructions());

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  vm->ApplyPatch(AsConstBytes(patch));

  patch = SamplePackets::PatchWithDelOperation().SerializeAsString();
  vm->ApplyPatch(AsConstBytes(patch));

  protos::TraceEntry state{};
  state.ParseFromString(SerializeIncrementalStateAsString(*vm));
  ASSERT_EQ(state.elements_size(), 1);
  ASSERT_EQ(state.elements(0).id(), 1);
  ASSERT_EQ(state.elements(0).value(), 11);
}

TEST_F(VmTest, ApplyPatch_MergeOperation) {
  auto vm = CreateVm(SamplePrograms::IncrementalTraceInstructions());

  // patch #1
  {
    auto patch = SamplePackets::PatchWithMergeOperation1().SerializeAsString();
    vm->ApplyPatch(AsConstBytes(patch));

    protos::TraceEntry state{};
    state.ParseFromString(SerializeIncrementalStateAsString(*vm));
    ASSERT_EQ(state.elements_size(), 1);
    ASSERT_EQ(state.elements(0).id(), 0);
    ASSERT_EQ(state.elements(0).value(), 10);
  }

  // patch #2
  {
    auto patch = SamplePackets::PatchWithMergeOperation2().SerializeAsString();
    vm->ApplyPatch(AsConstBytes(patch));

    protos::TraceEntry state{};
    state.ParseFromString(SerializeIncrementalStateAsString(*vm));
    ASSERT_EQ(state.elements_size(), 2);
    ASSERT_EQ(state.elements(0).id(), 0);
    ASSERT_EQ(state.elements(0).value(), 100);
    ASSERT_EQ(state.elements(1).id(), 1);
    ASSERT_EQ(state.elements(1).value(), 101);
  }
}

TEST_F(VmTest, ApplyPatch_SetOperation) {
  auto vm = CreateVm(SamplePrograms::IncrementalTraceInstructions());

  // empty
  {
    protos::TraceEntry state{};
    state.ParseFromString(SerializeIncrementalStateAsString(*vm));
    ASSERT_EQ(state.elements_size(), 0);
  }

  // patch #1
  {
    auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
    vm->ApplyPatch(AsConstBytes(patch));

    protos::TraceEntry state{};
    state.ParseFromString(SerializeIncrementalStateAsString(*vm));
    ASSERT_EQ(state.elements_size(), 2);
    ASSERT_EQ(state.elements(0).id(), 0);
    ASSERT_EQ(state.elements(0).value(), 10);
    ASSERT_EQ(state.elements(1).id(), 1);
    ASSERT_EQ(state.elements(1).value(), 11);
  }

  // patch #2
  {
    auto patch = SamplePackets::PatchWithSetOperation().SerializeAsString();
    vm->ApplyPatch(AsConstBytes(patch));

    protos::TraceEntry state{};
    state.ParseFromString(SerializeIncrementalStateAsString(*vm));
    ASSERT_EQ(state.elements_size(), 2);
    ASSERT_EQ(state.elements(0).id(), 0);
    ASSERT_FALSE(state.elements(0).has_value());
    ASSERT_EQ(state.elements(1).id(), 1);
    ASSERT_EQ(state.elements(1).value(), 101);
  }
}

TEST_F(VmTest, ApplyPatch_ErrorHandling) {
  auto vm = CreateVm(SamplePrograms::IncrementalTraceInstructions());

  auto patch = SamplePackets::PatchInconsistentWithIncrementalTraceProgram();
  auto status = vm->ApplyPatch(AsConstBytes(patch));
  ASSERT_TRUE(status.IsAbort());

  const auto& stacktrace = status.stacktrace();
  ASSERT_FALSE(stacktrace.empty());
  ASSERT_NE(stacktrace.front().find(
                "Attempted to access length-delimited field as a scalar"),
            std::string::npos);
}

TEST_F(VmTest, CloneReadOnly) {
  auto vm = CreateVm(SamplePrograms::IncrementalTraceInstructions());

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  vm->ApplyPatch(AsConstBytes(patch));

  std::unique_ptr<Vm> cloned_vm = vm->CloneReadOnly();

  // Check read-only VM doesn't accept patches
  ASSERT_TRUE(cloned_vm->ApplyPatch(AsConstBytes(patch)).IsAbort());

  // Check cloned incremental state
  protos::TraceEntry cloned_state{};
  cloned_state.ParseFromString(SerializeIncrementalStateAsString(*cloned_vm));
  ASSERT_EQ(cloned_state.elements_size(), 2);
  ASSERT_EQ(cloned_state.elements(0).id(), 0);
  ASSERT_EQ(cloned_state.elements(0).value(), 10);
  ASSERT_EQ(cloned_state.elements(1).id(), 1);
  ASSERT_EQ(cloned_state.elements(1).value(), 11);
}

TEST_F(VmTest, ApplyPatch_DelAliasedRoot) {
  auto vm = CreateVm(SamplePrograms::DelAliasedRoot());

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  auto status = vm->ApplyPatch(AsConstBytes(patch));
  ASSERT_TRUE(status.IsOk());
}

TEST_F(VmTest, ApplyPatch_DelAliasedDstAborts1) {
  auto vm = CreateVm(SamplePrograms::DelAliasedDstInsideSrcSelect());

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  auto status = vm->ApplyPatch(AsConstBytes(patch));
  ASSERT_TRUE(status.IsAbort());
}

TEST_F(VmTest, ApplyPatch_DelAliasedDstAborts2) {
  auto vm = CreateVm(SamplePrograms::DelAliasedDstInsideEmptyPathDstSelect());

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  auto status = vm->ApplyPatch(AsConstBytes(patch));
  ASSERT_TRUE(status.IsAbort());
}

TEST_F(VmTest, ApplyPatch_AccessDeletedDstAborts) {
  auto vm = CreateVm(SamplePrograms::DelAndNestedDel());

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  auto status = vm->ApplyPatch(AsConstBytes(patch));
  ASSERT_TRUE(status.IsAbort());
}

TEST_F(VmTest, GetMemoryUsage) {
  auto proto_program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  auto decoded_program = ProgramDecoder::Decode(AsConstBytes(proto_program));
  ASSERT_TRUE(decoded_program.IsOk());
  uint64_t expected_program_bytes =
      proto_program.size() + decoded_program->GetMemoryUsageBytes();
  auto vm = CreateVm(SamplePrograms::IncrementalTraceInstructions());

  // Initial memory usage only accounts for the program size
  ASSERT_EQ(vm->GetMemoryUsageBytes(), expected_program_bytes);
  ASSERT_EQ(vm->CloneReadOnly()->GetMemoryUsageBytes(), expected_program_bytes);

  // Populating the incremental state increases memory usage
  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  vm->ApplyPatch(AsConstBytes(patch));
  ASSERT_GT(vm->GetMemoryUsageBytes(), expected_program_bytes);
  ASSERT_GT(vm->CloneReadOnly()->GetMemoryUsageBytes(), expected_program_bytes);
}

TEST_F(VmTest, SerializeProgram) {
  auto proto_program = SamplePrograms::IncrementalTraceInstructions();
  auto vm = CreateVm(proto_program);
  ASSERT_EQ(vm->SerializeProgram(), proto_program.SerializeAsString());
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
