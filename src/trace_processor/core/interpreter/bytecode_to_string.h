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

#ifndef SRC_TRACE_PROCESSOR_CORE_INTERPRETER_BYTECODE_TO_STRING_H_
#define SRC_TRACE_PROCESSOR_CORE_INTERPRETER_BYTECODE_TO_STRING_H_

#include <cstdint>
#include <string>
#include <vector>

#include "src/trace_processor/core/common/sort_types.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/interpreter/interpreter_types.h"

namespace perfetto::trace_processor::core::interpreter {

struct Bytecode;

// Appends "name=value" for a bytecode argument to `fields`, skipping padding.
void AddField(const char* name, uint32_t value, std::vector<std::string>*);
void AddField(const char* name, const HandleBase&, std::vector<std::string>*);
void AddField(const char* name, NonNullOp, std::vector<std::string>*);
void AddField(const char* name, FilterValueHandle, std::vector<std::string>*);
void AddField(const char* name, BoundModifier, std::vector<std::string>*);
void AddField(const char* name, SortDirection, std::vector<std::string>*);
void AddField(const char* name, NullsLocation, std::vector<std::string>*);
std::string BytecodeFieldsFormat(const std::vector<std::string>& fields);

// Converts a bytecode instruction to string representation.
std::string ToString(const Bytecode& op);

}  // namespace perfetto::trace_processor::core::interpreter

#endif  // SRC_TRACE_PROCESSOR_CORE_INTERPRETER_BYTECODE_TO_STRING_H_
