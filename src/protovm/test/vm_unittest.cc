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
};

TEST_F(VmTest, NoPatch) {
  auto program =
      SamplePrograms::IncrementalTraceInstructions().SerializeAsString();
  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES};
  ASSERT_TRUE(vm.SerializeIncrementalState().empty());
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
  state.ParseFromString(vm.SerializeIncrementalState());
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
    state.ParseFromString(vm.SerializeIncrementalState());
    ASSERT_EQ(state.elements_size(), 1);
    ASSERT_EQ(state.elements(0).id(), 0);
    ASSERT_EQ(state.elements(0).value(), 10);
  }

  // patch #2
  {
    auto patch = SamplePackets::PatchWithMergeOperation2().SerializeAsString();
    vm.ApplyPatch(AsConstBytes(patch));

    protos::TraceEntry state{};
    state.ParseFromString(vm.SerializeIncrementalState());
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
    state.ParseFromString(vm.SerializeIncrementalState());
    ASSERT_EQ(state.elements_size(), 0);
  }

  // patch #1
  {
    auto patch = SamplePackets::PatchWithInitialState().SerializeAsString();
    vm.ApplyPatch(AsConstBytes(patch));

    protos::TraceEntry state{};
    state.ParseFromString(vm.SerializeIncrementalState());
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
    state.ParseFromString(vm.SerializeIncrementalState());
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

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
