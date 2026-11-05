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

#ifndef SRC_PROTOVM_PROGRAM_ENCODER_H_
#define SRC_PROTOVM_PROGRAM_ENCODER_H_

#include "protos/perfetto/protovm/vm_program.pbzero.h"

#include "src/protovm/program.h"

namespace perfetto {
namespace protovm {

class ProgramEncoder {
 public:
  static std::string Encode(const Program& program);

 private:
  static void EncodeInstruction(
      const Program& program,
      const Instruction& instruction,
      protos::pbzero::VmInstruction* proto_instruction);
  static void EncodePathComponents(
      base::Span<const PathComponent> path_components,
      protos::pbzero::VmOpSelect* proto_select);
};

}  // namespace protovm
}  // namespace perfetto

#endif  // SRC_PROTOVM_PROGRAM_ENCODER_H_
