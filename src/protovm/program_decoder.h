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

#ifndef SRC_PROTOVM_PROGRAM_DECODER_H_
#define SRC_PROTOVM_PROGRAM_DECODER_H_

#include "protos/perfetto/protovm/vm_program.pbzero.h"

#include "src/protovm/error_handling.h"
#include "src/protovm/program.h"

namespace perfetto {
namespace protovm {

class ProgramDecoder {
 public:
  // Each recursive frame of CountInstructionsAndPathComponents and
  // DecodeInstructions holds a protozero::TypedProtoDecoder with >1KB of
  // inlined on stack storage. So let's enforce a recursion limit to avoid
  // untrusted input to cause stack overflows.
  static constexpr uint32_t kMaxInstructionNestingDepth = 128;

  static StatusOr<Program> Decode(protozero::ConstBytes proto_bytes);

 private:
  struct CountResult {
    uint64_t instructions{0};
    uint64_t path_components{0};
  };

  static StatusOr<CountResult> CountInstructionsAndPathComponents(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it,
      uint32_t depth = 0);
  static uint64_t CountPathComponents(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it);
  static bool CanInlinePathIntoInstruction(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> path_it);

  StatusOr<base::Span<const Instruction>> DecodeInstructions(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it);
  StatusOr<Instruction> DecodeInstruction(
      const protos::pbzero::VmInstruction::Decoder& proto_instruction);
  StatusOr<Instruction> DecodeSelect(
      const protos::pbzero::VmOpSelect::Decoder& proto_select,
      AbortLevel abort_level);
  StatusOr<Instruction> DecodeRegLoad(
      const protos::pbzero::VmOpRegLoad::Decoder& proto_reg_load,
      AbortLevel abort_level);
  StatusOr<Instruction> DecodeMerge(
      const protos::pbzero::VmOpMerge::Decoder& proto_merge,
      AbortLevel abort_level);
  StatusOr<Instruction> DecodeSet(AbortLevel abort_level);
  StatusOr<Instruction> DecodeDel(AbortLevel abort_level);
  StatusOr<base::Span<const PathComponent>> DecodePathComponents(
      protozero::RepeatedFieldIterator<protozero::ConstBytes> proto_it,
      Cursor selected_cursor);

  Program program_;
};

}  // namespace protovm
}  // namespace perfetto

#endif  // SRC_PROTOVM_PROGRAM_DECODER_H_
