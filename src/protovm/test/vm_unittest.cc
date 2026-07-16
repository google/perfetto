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
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};
  ASSERT_TRUE(SerializeIncrementalStateAsString(vm).empty());
}

TEST_F(VmTest, ConstructionWithInitialIncrementalState) {
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();

  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES,
        AsConstBytes(InitialIncrementalState())};

  protos::TraceEntry state{};
  state.ParseFromString(SerializeIncrementalStateAsString(vm));
  ASSERT_EQ(state.elements_size(), 2);
  ASSERT_EQ(state.elements(0).id(), 0);
  ASSERT_EQ(state.elements(0).value(), 10);
  ASSERT_EQ(state.elements(1).id(), 1);
  ASSERT_EQ(state.elements(1).value(), 11);
}

TEST_F(VmTest, ApplyPatch_DelOperation) {
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  vm.ApplyPatch(AsConstBytes(patch));

  patch = SamplePackets::PatchWithDelOperation().SerializeAsString();
  vm.ApplyPatch(AsConstBytes(patch));

  protos::TraceEntry state{};
  state.ParseFromString(SerializeIncrementalStateAsString(vm));
  ASSERT_EQ(state.elements_size(), 1);
  ASSERT_EQ(state.elements(0).id(), 1);
  ASSERT_EQ(state.elements(0).value(), 11);
}

TEST_F(VmTest, ApplyPatch_MergeOperation) {
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};

  // patch #1
  {
    auto patch = SamplePackets::PatchWithMergeOperation1().SerializeAsString();
    vm.ApplyPatch(AsConstBytes(patch));

    protos::TraceEntry state{};
    state.ParseFromString(SerializeIncrementalStateAsString(vm));
    ASSERT_EQ(state.elements_size(), 1);
    ASSERT_EQ(state.elements(0).id(), 0);
    ASSERT_EQ(state.elements(0).value(), 10);
  }

  // patch #2
  {
    auto patch = SamplePackets::PatchWithMergeOperation2().SerializeAsString();
    vm.ApplyPatch(AsConstBytes(patch));

    protos::TraceEntry state{};
    state.ParseFromString(SerializeIncrementalStateAsString(vm));
    ASSERT_EQ(state.elements_size(), 2);
    ASSERT_EQ(state.elements(0).id(), 0);
    ASSERT_EQ(state.elements(0).value(), 100);
    ASSERT_EQ(state.elements(1).id(), 1);
    ASSERT_EQ(state.elements(1).value(), 101);
  }
}

TEST_F(VmTest, ApplyPatch_SetOperation) {
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};

  // empty
  {
    protos::TraceEntry state{};
    state.ParseFromString(SerializeIncrementalStateAsString(vm));
    ASSERT_EQ(state.elements_size(), 0);
  }

  // patch #1
  {
    auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
    vm.ApplyPatch(AsConstBytes(patch));

    protos::TraceEntry state{};
    state.ParseFromString(SerializeIncrementalStateAsString(vm));
    ASSERT_EQ(state.elements_size(), 2);
    ASSERT_EQ(state.elements(0).id(), 0);
    ASSERT_EQ(state.elements(0).value(), 10);
    ASSERT_EQ(state.elements(1).id(), 1);
    ASSERT_EQ(state.elements(1).value(), 11);
  }

  // patch #2
  {
    auto patch = SamplePackets::PatchWithSetOperation().SerializeAsString();
    vm.ApplyPatch(AsConstBytes(patch));

    protos::TraceEntry state{};
    state.ParseFromString(SerializeIncrementalStateAsString(vm));
    ASSERT_EQ(state.elements_size(), 2);
    ASSERT_EQ(state.elements(0).id(), 0);
    ASSERT_FALSE(state.elements(0).has_value());
    ASSERT_EQ(state.elements(1).id(), 1);
    ASSERT_EQ(state.elements(1).value(), 101);
  }
}

TEST_F(VmTest, ApplyPatch_ErrorHandling) {
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};

  auto patch = SamplePackets::PatchInconsistentWithIncrementalTraceProgram();
  auto status = vm.ApplyPatch(AsConstBytes(patch));
  ASSERT_TRUE(status.IsAbort());

  const auto& stacktrace = status.stacktrace();
  ASSERT_FALSE(stacktrace.empty());
  ASSERT_NE(stacktrace.front().find(
                "Attempted to access length-delimited field as a scalar"),
            std::string::npos);
}

TEST_F(VmTest, CloneReadOnly) {
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  vm.ApplyPatch(AsConstBytes(patch));

  std::unique_ptr<Vm> cloned_vm = vm.CloneReadOnly();

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
  auto program = SamplePrograms::DelAliasedRoot().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  auto status = vm.ApplyPatch(AsConstBytes(patch));
  ASSERT_TRUE(status.IsOk());
}

TEST_F(VmTest, ApplyPatch_DelAliasedDstAborts1) {
  auto program =
      SamplePrograms::DelAliasedDstInsideSrcSelect().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  auto status = vm.ApplyPatch(AsConstBytes(patch));
  ASSERT_TRUE(status.IsAbort());
}

TEST_F(VmTest, ApplyPatch_DelAliasedDstAborts2) {
  auto program = SamplePrograms::DelAliasedDstInsideEmptyPathDstSelect()
                     .SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  auto status = vm.ApplyPatch(AsConstBytes(patch));
  ASSERT_TRUE(status.IsAbort());
}

TEST_F(VmTest, ApplyPatch_AccessDeletedDstAborts) {
  auto program = SamplePrograms::DelAndNestedDel().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  auto status = vm.ApplyPatch(AsConstBytes(patch));
  ASSERT_TRUE(status.IsAbort());
}

TEST_F(VmTest, ApplyPatch_FailedPatchKeepsPreviousState) {
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  constexpr size_t kSmallMemoryLimitBytes = 4096;
  Vm vm{AsConstBytes(program), kSmallMemoryLimitBytes};

  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  ASSERT_TRUE(vm.ApplyPatch(AsConstBytes(patch)).IsOk());
  std::string state_before_failed_patch = SerializeIncrementalStateAsString(vm);

  // Craft a patch whose first elements_to_merge entry fits in memory and
  // whose second entry exceeds the memory limit, so that the program fails
  // after having already merged the first entry.
  protozero::HeapBuffered<protozero::Message> failing_patch;
  {
    auto* element = failing_patch->BeginNestedMessage<protozero::Message>(
        protos::Patch::kElementsToMergeFieldNumber);
    element->AppendVarInt(protos::Element::kIdFieldNumber, 0);
    element->AppendVarInt(protos::Element::kValueFieldNumber, 100);
    element->Finalize();
  }
  {
    auto* element = failing_patch->BeginNestedMessage<protozero::Message>(
        protos::Patch::kElementsToMergeFieldNumber);
    element->AppendVarInt(protos::Element::kIdFieldNumber, 1);
    std::string huge_payload(2 * kSmallMemoryLimitBytes, 'x');
    element->AppendBytes(1000, huge_payload.data(), huge_payload.size());
    element->Finalize();
  }

  auto failing_patch_bytes = failing_patch.SerializeAsString();
  ASSERT_FALSE(vm.ApplyPatch(AsConstBytes(failing_patch_bytes)).IsOk());

  // The failed patch must be rolled back entirely: a half-applied patch would
  // silently corrupt the incremental state.
  ASSERT_EQ(SerializeIncrementalStateAsString(vm), state_before_failed_patch);
}

TEST_F(VmTest, ApplyPatch_TruncatedPatchFailsAndKeepsPreviousState) {
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES,
        AsConstBytes(InitialIncrementalState())};

  std::string state_before_patch = SerializeIncrementalStateAsString(vm);

  auto patch = SamplePackets::PatchWithMergeOperation1().SerializeAsString();
  auto truncated_patch = patch.substr(0, patch.size() - 1);
  ASSERT_FALSE(vm.ApplyPatch(AsConstBytes(truncated_patch)).IsOk());

  ASSERT_EQ(SerializeIncrementalStateAsString(vm), state_before_patch);
}

// Documents a known limitation of the schema-free VM: a packet that is not a
// patch, but whose field numbers happen to collide with the ones the program
// selects on, is folded into the incremental state with an OK status. Here
// TraceEntry.single_element (field 2) collides with Patch.elements_to_merge
// (field 2). Full prevention requires schema knowledge (or a system-level
// marker restricting which packets reach the VM), which the VM doesn't have.
TEST_F(VmTest, ApplyPatch_ForeignPacketWithCollidingFieldIdsIsAbsorbed) {
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};

  protos::TraceEntry foreign_packet;
  foreign_packet.mutable_single_element()->set_id(7);
  foreign_packet.mutable_single_element()->set_value(42);
  auto foreign_packet_bytes = foreign_packet.SerializeAsString();

  ASSERT_TRUE(vm.ApplyPatch(AsConstBytes(foreign_packet_bytes)).IsOk());

  protos::TraceEntry state{};
  state.ParseFromString(SerializeIncrementalStateAsString(vm));
  ASSERT_EQ(state.elements_size(), 1);
  ASSERT_EQ(state.elements(0).id(), 7);
  ASSERT_EQ(state.elements(0).value(), 42);
}

TEST_F(VmTest, ApplyPatch_MapKeysAbove32BitAreDistinct) {
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};

  // Insert an element whose key doesn't fit in 32 bits (5 + 2^32) and then an
  // element with key 5. If the key is truncated to 32 bits along the way, the
  // two patches collide on the same map entry.
  constexpr uint64_t kBigId = 5 + (uint64_t{1} << 32);
  {
    protozero::HeapBuffered<protozero::Message> patch;
    auto* element = patch->BeginNestedMessage<protozero::Message>(
        protos::Patch::kElementsToSetFieldNumber);
    element->AppendVarInt(protos::Element::kIdFieldNumber, kBigId);
    element->AppendVarInt(protos::Element::kValueFieldNumber, 1);
    element->Finalize();
    auto patch_bytes = patch.SerializeAsString();
    ASSERT_TRUE(vm.ApplyPatch(AsConstBytes(patch_bytes)).IsOk());
  }
  {
    protozero::HeapBuffered<protozero::Message> patch;
    auto* element = patch->BeginNestedMessage<protozero::Message>(
        protos::Patch::kElementsToSetFieldNumber);
    element->AppendVarInt(protos::Element::kIdFieldNumber, 5);
    element->AppendVarInt(protos::Element::kValueFieldNumber, 2);
    element->Finalize();
    auto patch_bytes = patch.SerializeAsString();
    ASSERT_TRUE(vm.ApplyPatch(AsConstBytes(patch_bytes)).IsOk());
  }

  protos::TraceEntry state{};
  state.ParseFromString(SerializeIncrementalStateAsString(vm));
  ASSERT_EQ(state.elements_size(), 2);
}

TEST_F(VmTest, ApplyPatch_RegisterIdOutOfRangeAborts) {
  // reg_load with dst_register = 256. A naive truncation to uint8_t would
  // silently alias register 0 instead of failing.
  perfetto::protos::VmProgram program;
  auto* instruction = program.add_instructions();
  auto* select = instruction->mutable_select();
  select->add_relative_path()->set_field_id(
      protos::Patch::kElementsToMergeFieldNumber);
  select->add_relative_path()->set_array_index(0);
  select->add_relative_path()->set_field_id(protos::Element::kIdFieldNumber);
  auto* reg_load = instruction->add_nested_instructions()->mutable_reg_load();
  reg_load->set_dst_register(256);

  auto program_bytes = program.SerializeAsString();
  Vm vm{AsConstBytes(program_bytes), MEMORY_LIMIT_BYTES};

  auto patch = SamplePackets::PatchWithMergeOperation1().SerializeAsString();
  ASSERT_TRUE(vm.ApplyPatch(AsConstBytes(patch)).IsAbort());
}

TEST_F(VmTest, GetMemoryUsage) {
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};

  // Initial memory usage only accounts for the program size
  ASSERT_EQ(vm.GetMemoryUsageBytes(), program.size());
  ASSERT_EQ(vm.CloneReadOnly()->GetMemoryUsageBytes(), program.size());

  // Populating the incremental state increases memory usage
  auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
  vm.ApplyPatch(AsConstBytes(patch));
  ASSERT_GT(vm.GetMemoryUsageBytes(), program.size());
  ASSERT_GT(vm.CloneReadOnly()->GetMemoryUsageBytes(), program.size());
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
