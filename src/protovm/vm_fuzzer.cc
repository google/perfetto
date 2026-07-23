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

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "perfetto/protozero/message.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/protovm/vm_program.pbzero.h"
#include "src/protovm/vm.h"

namespace perfetto {
namespace protovm {
namespace {

constexpr size_t kMemoryLimitBytes = 64 * 1024;

// Field ids used by the sample program and the sample patch. They mirror the
// test schema in src/protovm/test/protos/incremental_trace.proto:
//   Patch.elements_to_delete = 1 (repeated int32)
//   Patch.elements_to_merge = 2 (repeated Element)
//   Patch.elements_to_set = 3 (repeated Element)
//   Element.id = 1, Element.value = 2
//   TraceEntry.elements = 1 (repeated Element)
constexpr uint32_t kPatchElementsToDelete = 1;
constexpr uint32_t kPatchElementsToMerge = 2;
constexpr uint32_t kPatchElementsToSet = 3;
constexpr uint32_t kElementId = 1;
constexpr uint32_t kElementValue = 2;
constexpr uint32_t kTraceEntryElements = 1;
constexpr uint32_t kRegisterHoldingElementId = 0;

// Builds the same del/merge/set program as
// SamplePrograms::IncrementalTraceInstructions() (test-only code that can't be
// referenced from here).
std::string BuildSampleProgram() {
  protozero::HeapBuffered<protos::pbzero::VmProgram> program;

  // Process elements_to_delete.
  {
    auto* instr_src_select = program->add_instructions();
    auto* src_select = instr_src_select->set_select();
    auto* src_element = src_select->add_relative_path();
    src_element->set_field_id(kPatchElementsToDelete);
    src_element->set_is_repeated(true);

    auto* instr_reg_load = instr_src_select->add_nested_instructions();
    instr_reg_load->set_reg_load()->set_dst_register(kRegisterHoldingElementId);

    auto* instr_dst_select = instr_src_select->add_nested_instructions();
    auto* dst_select = instr_dst_select->set_select();
    dst_select->set_cursor(protos::pbzero::VmCursorEnum::VM_CURSOR_DST);
    dst_select->set_create_if_not_exist(false);
    dst_select->add_relative_path()->set_field_id(kTraceEntryElements);
    auto* component_map_key = dst_select->add_relative_path();
    component_map_key->set_map_key_field_id(kElementId);
    component_map_key->set_register_to_match(kRegisterHoldingElementId);

    instr_dst_select->add_nested_instructions()->set_del();
  }

  // Process elements_to_merge and elements_to_set.
  struct {
    uint32_t src_field_id;
    bool is_merge;
  } const ops[] = {{kPatchElementsToMerge, true}, {kPatchElementsToSet, false}};
  for (const auto& op : ops) {
    auto* instr_src_select = program->add_instructions();
    auto* src_select = instr_src_select->set_select();
    auto* src_element = src_select->add_relative_path();
    src_element->set_field_id(op.src_field_id);
    src_element->set_is_repeated(true);

    auto* instr_src_select_id = instr_src_select->add_nested_instructions();
    instr_src_select_id->set_select()->add_relative_path()->set_field_id(
        kElementId);
    instr_src_select_id->add_nested_instructions()
        ->set_reg_load()
        ->set_dst_register(kRegisterHoldingElementId);

    auto* instr_dst_select = instr_src_select->add_nested_instructions();
    auto* dst_select = instr_dst_select->set_select();
    dst_select->set_cursor(protos::pbzero::VmCursorEnum::VM_CURSOR_DST);
    dst_select->set_create_if_not_exist(true);
    dst_select->add_relative_path()->set_field_id(kTraceEntryElements);
    auto* component_map_key = dst_select->add_relative_path();
    component_map_key->set_map_key_field_id(kElementId);
    component_map_key->set_register_to_match(kRegisterHoldingElementId);

    auto* instr_op = instr_dst_select->add_nested_instructions();
    if (op.is_merge) {
      instr_op->set_merge();
    } else {
      instr_op->set_set();
    }
  }

  return program.SerializeAsString();
}

std::string BuildSamplePatch() {
  protozero::HeapBuffered<protozero::Message> patch;
  for (uint64_t id = 0; id < 2; ++id) {
    auto* element =
        patch->BeginNestedMessage<protozero::Message>(kPatchElementsToSet);
    element->AppendVarInt(kElementId, id);
    element->AppendVarInt(kElementValue, id * 10);
    element->Finalize();
  }
  patch->AppendVarInt(kPatchElementsToDelete, 0);
  return patch.SerializeAsString();
}

protozero::ConstBytes AsConstBytes(const std::string& s) {
  return protozero::ConstBytes{reinterpret_cast<const uint8_t*>(s.data()),
                               s.size()};
}

void SerializeState(const Vm& vm) {
  protozero::HeapBuffered<protozero::Message> out;
  vm.SerializeIncrementalState(out.get());
  out.SerializeAsString();
}

int FuzzProtoVm(const uint8_t* data, size_t size) {
  static const std::string& sample_program =
      *new std::string(BuildSampleProgram());
  static const std::string& sample_patch = *new std::string(BuildSamplePatch());

  // Fixed program, fuzzed patch bytes. Apply twice so that the second patch
  // also exercises a non-empty incremental state.
  {
    Vm vm{AsConstBytes(sample_program), kMemoryLimitBytes};
    vm.ApplyPatch(protozero::ConstBytes{data, size});
    vm.ApplyPatch(protozero::ConstBytes{data, size});
    SerializeState(vm);
    SerializeState(*vm.CloneReadOnly());
    vm.GetMemoryUsageBytes();
  }

  // Fuzzed program and initial state, fixed patch.
  {
    Vm vm{protozero::ConstBytes{data, size}, kMemoryLimitBytes,
          protozero::ConstBytes{data, size}};
    vm.ApplyPatch(AsConstBytes(sample_patch));
    SerializeState(vm);
    vm.GetMemoryUsageBytes();
  }

  return 0;
}

}  // namespace
}  // namespace protovm
}  // namespace perfetto

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  return perfetto::protovm::FuzzProtoVm(data, size);
}
