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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSIC_HELPERS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSIC_HELPERS_H_

#include <memory>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"

namespace perfetto::trace_processor::plugins {

// =============================================================================
// Pointer type helpers - use T::kPointerType for type-safe pointer handling
// =============================================================================

// Get a pointer from sqlite3_value using the type's kPointerType.
// Returns nullptr if the pointer type doesn't match.
template <typename T>
T* GetPointer(sqlite3_value* value) {
  return sqlite::value::Pointer<T>(value, T::kPointerType);
}

// Get a pointer with error handling using the type's kPointerType.
// Returns error status if the pointer is null or type doesn't match.
template <typename T>
base::StatusOr<T*> ExpectPointer(sqlite3_value* value, const char* func_name) {
  auto* ptr = sqlite::value::Pointer<T>(value, T::kPointerType);
  if (!ptr) {
    return base::ErrStatus("%s: expected %s", func_name, T::kPointerType);
  }
  return ptr;
}

// Return an existing unique_ptr as SQLite result using the type's kPointerType.
template <typename T>
void UniquePtrResult(sqlite3_context* ctx, std::unique_ptr<T> ptr) {
  sqlite::result::UniquePointer(ctx, std::move(ptr), T::kPointerType);
}

// Create and return a unique_ptr as SQLite result using the type's
// kPointerType.
template <typename T, typename... Args>
void MakeUniquePtrResult(sqlite3_context* ctx, Args&&... args) {
  UniquePtrResult(ctx, std::make_unique<T>(std::forward<Args>(args)...));
}

}  // namespace perfetto::trace_processor::plugins

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSIC_HELPERS_H_
