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

#include <cstdint>

#include "perfetto/protozero/field.h"

#include "src/protovm/error_handling.h"
#include "src/protovm/program.h"

namespace perfetto {
namespace protovm {

// Limit recursive decoding to prevent stack overflows on untrusted input.
constexpr uint32_t kMaxInstructionNestingDepth = 128;

StatusOr<Program> DecodeProgram(protozero::ConstBytes proto_bytes);

}  // namespace protovm
}  // namespace perfetto

#endif  // SRC_PROTOVM_PROGRAM_DECODER_H_
