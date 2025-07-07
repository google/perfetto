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

#include "src/profiling/symbolizer/llvm_symbolizer.h"

#include <dlfcn.h>

#include <utility>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace profiling {

// dlclose() was not used as it rarely works and is flaky
SymbolizationResultBatch::SymbolizationResultBatch(
    BatchSymbolizationResult c_api_result,
    decltype(&::LlvmSymbolizer_FreeBatchSymbolizationResult) free_fn)
    : c_api_result_(c_api_result), free_result_fn_(free_fn) {
  if (c_api_result_.results == nullptr) {
    return;
  }
  results_.reserve(c_api_result.num_results);
  for (size_t i = 0; i < c_api_result.num_results; ++i) {
    const SymbolizationResult& result = c_api_result.results[i];
    std::vector<LlvmSymbolizedFrame> frames;
    if (result.frames) {
      frames.reserve(result.num_frames);
      for (size_t j = 0; j < result.num_frames; ++j) {
        frames.emplace_back(LlvmSymbolizedFrame{result.frames[j].function_name,
                                                result.frames[j].file_name,
                                                result.frames[j].line_number});
      }
    }
    results_.push_back(std::move(frames));
  }
}

void SymbolizationResultBatch::Free() {
  if (c_api_result_.results && free_result_fn_) {
    free_result_fn_(c_api_result_);
  }
  c_api_result_ = {};
}

SymbolizationResultBatch::~SymbolizationResultBatch() {
  Free();
}

SymbolizationResultBatch::SymbolizationResultBatch(
    SymbolizationResultBatch&& other) noexcept
    : c_api_result_(std::exchange(other.c_api_result_, {})),
      free_result_fn_(std::exchange(other.free_result_fn_, nullptr)),
      results_(std::move(other.results_)) {}

SymbolizationResultBatch& SymbolizationResultBatch::operator=(
    SymbolizationResultBatch&& other) noexcept {
  if (this != &other) {
    Free();
    c_api_result_ = std::exchange(other.c_api_result_, {});
    free_result_fn_ = std::exchange(other.free_result_fn_, nullptr);
    results_ = std::move(other.results_);
  }
  return *this;
}

LlvmSymbolizer::LlvmSymbolizer() {
  library_handle_.reset(dlopen("libllvm_symbolizer_wrapper.so", RTLD_NOW));
  if (!library_handle_) {
    PERFETTO_ELOG("Failed to open libllvm_symbolizer_wrapper.so: %s",
                  dlerror());
    return;
  }

  create_fn_ = reinterpret_cast<decltype(create_fn_)>(
      dlsym(*library_handle_, "LlvmSymbolizer_Create"));
  destroy_fn_ = reinterpret_cast<decltype(destroy_fn_)>(
      dlsym(*library_handle_, "LlvmSymbolizer_Destroy"));
  symbolize_fn_ = reinterpret_cast<decltype(symbolize_fn_)>(
      dlsym(*library_handle_, "LlvmSymbolizer_Symbolize"));
  free_result_fn_ = reinterpret_cast<decltype(free_result_fn_)>(
      dlsym(*library_handle_, "LlvmSymbolizer_FreeBatchSymbolizationResult"));

  if (!create_fn_ || !destroy_fn_ || !symbolize_fn_ || !free_result_fn_) {
    PERFETTO_ELOG("Failed to look up symbols in libllvm_symbolizer_wrapper.so");
    library_handle_.reset();  // Release the handle on failure.
    return;
  }

  c_api_symbolizer_ = create_fn_();
  if (!c_api_symbolizer_) {
    PERFETTO_ELOG("LlvmSymbolizer_Create() failed.");
    library_handle_.reset();
    create_fn_ = nullptr;
    return;
  }
}

LlvmSymbolizer::~LlvmSymbolizer() {
  if (c_api_symbolizer_) {
    destroy_fn_(c_api_symbolizer_);
  }
}

SymbolizationResultBatch LlvmSymbolizer::SymbolizeBatch(
    const std::vector<SymbolizationRequest>& requests) {
  if (!c_api_symbolizer_) {
    return SymbolizationResultBatch({}, free_result_fn_);
  }

  std::vector<::SymbolizationRequest> c_requests;
  c_requests.reserve(requests.size());
  for (const auto& request : requests) {
    c_requests.emplace_back(
        ::SymbolizationRequest{request.binary.c_str(), request.address});
  }

  BatchSymbolizationResult batch_result =
      symbolize_fn_(c_api_symbolizer_, c_requests.data(), c_requests.size());
  return SymbolizationResultBatch(batch_result, free_result_fn_);
}

}  // namespace profiling
}  // namespace perfetto
