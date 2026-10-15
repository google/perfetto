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

#ifndef SRC_TRACING_TEST_PROTOVM_TEST_PROGRAM_H_
#define SRC_TRACING_TEST_PROTOVM_TEST_PROGRAM_H_

#include <stdint.h>

#include "protos/perfetto/protovm/vm_program.gen.h"

namespace perfetto {

// Returns a ProtoVM program for tests.
// For each packet it processes, it copies protovm_patch.string_to_merge and
// int_to_merge into its state as protovm_incremental_state.string_merged and
// int_merged, so the state holds the values of the last packet.
// Two programs with different |program_version| run as two ProtoVMs.
protos::gen::VmProgram CreateProtoVmProgram(uint32_t program_version = 1);

}  // namespace perfetto

#endif  // SRC_TRACING_TEST_PROTOVM_TEST_PROGRAM_H_
