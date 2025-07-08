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

#ifndef SRC_PROFILING_SYMBOLIZER_LLVM_SYMBOLIZER_H_
#define SRC_PROFILING_SYMBOLIZER_LLVM_SYMBOLIZER_H_

#include <utility>
#include <vector>

#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_view.h"
#include "src/profiling/symbolizer/llvm_symbolizer_c_api.h"

namespace perfetto {
namespace profiling {
class LlvmSymbolizer;

struct SymbolizationRequest {
  std::string binary;
  uint64_t address;
};

struct LlvmSymbolizedFrame {
  const char* function_name;
  const char* file_name;
  uint32_t line_number;
};

// RAII wrapper for the results of a batch symbolization.
// This object owns the single contiguous block of memory returned by the C API
// and provides safe, non-owning views to the symbolized frames.
class SymbolizationResultBatch {
 public:
  ~SymbolizationResultBatch();

  // This class is move-only to ensure unique ownership of the underlying data.
  SymbolizationResultBatch(const SymbolizationResultBatch&) = delete;
  SymbolizationResultBatch& operator=(const SymbolizationResultBatch&) = delete;
  SymbolizationResultBatch(SymbolizationResultBatch&&) noexcept;
  SymbolizationResultBatch& operator=(SymbolizationResultBatch&&) noexcept;

  // Returns a pair of (pointer, size) for the frames of a given request.
  std::pair<const LlvmSymbolizedFrame*, uint32_t> GetFramesForRequest(
      uint32_t request_index) const;

  // Returns the number of original requests.
  uint32_t size() const { return num_ranges_; }

 private:
  friend class LlvmSymbolizer;

  SymbolizationResultBatch(
      BatchSymbolizationResult c_api_result,
      decltype(&::LlvmSymbolizer_FreeBatchSymbolizationResult) free_fn);

  void Free();

  BatchSymbolizationResult c_api_result_{};
  decltype(&::LlvmSymbolizer_FreeBatchSymbolizationResult) free_result_fn_{};

  // Non-owning views into the C API's flat buffer, implemented with raw
  // pointers and sizes.
  const LlvmSymbolizedFrame* all_frames_ptr_ = nullptr;
  uint32_t num_total_frames_ = 0;

  const SymbolizationResultRange* ranges_ptr_ = nullptr;
  uint32_t num_ranges_ = 0;
};

namespace {
// A no-op closer function for dlopen handles, as dlclose() is flaky.
// ScopedResource requires a static function pointer for its template.
inline int NoOpDlclose(void* /*handle*/) {
  return 0;
}
}  // namespace

class LlvmSymbolizer {
 public:
  LlvmSymbolizer();
  ~LlvmSymbolizer();

  LlvmSymbolizer(const LlvmSymbolizer&) = delete;
  LlvmSymbolizer& operator=(const LlvmSymbolizer&) = delete;
  LlvmSymbolizer(LlvmSymbolizer&&) noexcept = default;
  LlvmSymbolizer& operator=(LlvmSymbolizer&&) noexcept = default;

  SymbolizationResultBatch SymbolizeBatch(
      const std::vector<SymbolizationRequest>& requests);

 private:
  using ScopedLibraryHandle = base::ScopedResource<void*, NoOpDlclose, nullptr>;

  ScopedLibraryHandle library_handle_;
  ::LlvmSymbolizer* c_api_symbolizer_ = nullptr;

  // C API function pointers
  decltype(&::LlvmSymbolizer_Create) create_fn_ = nullptr;
  decltype(&::LlvmSymbolizer_Destroy) destroy_fn_ = nullptr;
  decltype(&::LlvmSymbolizer_Symbolize) symbolize_fn_ = nullptr;
  decltype(&::LlvmSymbolizer_FreeBatchSymbolizationResult) free_result_fn_ =
      nullptr;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_SYMBOLIZER_LLVM_SYMBOLIZER_H_
