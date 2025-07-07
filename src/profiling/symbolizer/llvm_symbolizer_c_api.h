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

#ifndef SRC_PROFILING_SYMBOLIZER_LLVM_SYMBOLIZER_C_API_H_
#define SRC_PROFILING_SYMBOLIZER_LLVM_SYMBOLIZER_C_API_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LLVM_SYMBOLIZER_C_API __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to the underlying C++ symbolizer object.
struct LlvmSymbolizer;
typedef struct LlvmSymbolizer LlvmSymbolizer;

// Represents a single symbolization request.
typedef struct {
  const char* binary_path;
  uint64_t address;
} SymbolizationRequest;

// Represents a single symbolized stack frame.
typedef struct {
  const char* function_name;
  const char* file_name;
  uint32_t line_number;
} SymbolizedFrame;

// Represents the result of a single symbolization operation.
typedef struct {
  SymbolizedFrame* frames;
  size_t num_frames;
} SymbolizationResult;

// Represents the result of a batch of symbolization operations.
typedef struct {
  SymbolizationResult* results;
  size_t num_results;
} BatchSymbolizationResult;

// Creates an instance of the LLVM symbolizer.
// Returns NULL on failure.
LLVM_SYMBOLIZER_C_API LlvmSymbolizer* LlvmSymbolizer_Create(void);

// Destroys an instance of the LLVM symbolizer.
LLVM_SYMBOLIZER_C_API void LlvmSymbolizer_Destroy(LlvmSymbolizer* sym);

// Symbolizes a batch of addresses.
// The caller is responsible for freeing the result with
// LlvmSymbolizer_FreeBatchSymbolizationResult.
LLVM_SYMBOLIZER_C_API BatchSymbolizationResult
LlvmSymbolizer_Symbolize(LlvmSymbolizer* sym,
                         const SymbolizationRequest* requests,
                         size_t num_requests);

// Frees the memory allocated for a BatchSymbolizationResult.
LLVM_SYMBOLIZER_C_API void LlvmSymbolizer_FreeBatchSymbolizationResult(
    BatchSymbolizationResult result);

#ifdef __cplusplus
}
#endif

#endif  // SRC_PROFILING_SYMBOLIZER_LLVM_SYMBOLIZER_C_API_H_
