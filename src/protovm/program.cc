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

namespace perfetto {
namespace protovm {

StatusOr<void> Instruction::SetNestedInstructions(uint64_t offset,
                                                  uint64_t size) {
  switch (type()) {
    case InstructionType::kSelect:
    case InstructionType::kSelectInlinedFieldId:
    case InstructionType::kSelectInlinedRepeatedForEach:
      PROTOVM_CHECKED_CAST(select.nested_instructions_offset, offset,
                           "select nested instructions offset too large");
      PROTOVM_CHECKED_CAST(select.nested_instructions_size, size,
                           "select nested instructions size too large");
      break;
    case InstructionType::kRegLoad:
      PROTOVM_CHECKED_CAST(reg_load.nested_instructions_offset, offset,
                           "reg_load nested instructions offset too large");
      PROTOVM_CHECKED_CAST(reg_load.nested_instructions_size, size,
                           "reg_load nested instructions size too large");
      break;
    case InstructionType::kMerge:
      PROTOVM_CHECKED_CAST(merge.nested_instructions_offset, offset,
                           "merge nested instructions offset too large");
      PROTOVM_CHECKED_CAST(merge.nested_instructions_size, size,
                           "merge nested instructions size too large");
      break;
    case InstructionType::kSet:
      PROTOVM_CHECKED_CAST(set.nested_instructions_offset, offset,
                           "set nested instructions offset too large");
      PROTOVM_CHECKED_CAST(set.nested_instructions_size, size,
                           "set nested instructions size too large");
      break;
    case InstructionType::kDel:
      PROTOVM_CHECKED_CAST(del.nested_instructions_offset, offset,
                           "del nested instructions offset too large");
      PROTOVM_CHECKED_CAST(del.nested_instructions_size, size,
                           "del nested instructions size too large");
      break;
  }
  return StatusOr<void>::Ok();
}

}  // namespace protovm
}  // namespace perfetto
