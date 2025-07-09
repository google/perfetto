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
#include <vector>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace profiling {

// dlclose() was not used as it rarely works and is flaky
SymbolizationResultBatch::SymbolizationResultBatch(
    BatchSymbolizationResult c_api_result,
    decltype(&::LlvmSymbolizer_FreeBatchSymbolizationResult) free_fn)
    : c_api_result_(c_api_result), free_result_fn_(free_fn) {
  if (c_api_result.ranges) {
    all_frames_ptr_ = c_api_result.frames;
    num_total_frames_ = c_api_result.total_frames;
    ranges_ptr_ = c_api_result.ranges;
    num_ranges_ = c_api_result.num_ranges;
  }
}

SymbolizationResultBatch::~SymbolizationResultBatch() {
  Free();
}

SymbolizationResultBatch::SymbolizationResultBatch(
    SymbolizationResultBatch&& other) noexcept
    : c_api_result_(std::exchange(other.c_api_result_, {})),
      free_result_fn_(std::exchange(other.free_result_fn_, nullptr)),
      all_frames_ptr_(std::exchange(other.all_frames_ptr_, nullptr)),
      num_total_frames_(std::exchange(other.num_total_frames_, 0)),
      ranges_ptr_(std::exchange(other.ranges_ptr_, nullptr)),
      num_ranges_(std::exchange(other.num_ranges_, 0)) {}

SymbolizationResultBatch& SymbolizationResultBatch::operator=(
    SymbolizationResultBatch&& other) noexcept {
  if (this != &other) {
    Free();
    c_api_result_ = std::exchange(other.c_api_result_, {});
    free_result_fn_ = std::exchange(other.free_result_fn_, nullptr);
    all_frames_ptr_ = std::exchange(other.all_frames_ptr_, nullptr);
    num_total_frames_ = std::exchange(other.num_total_frames_, 0);
    ranges_ptr_ = std::exchange(other.ranges_ptr_, nullptr);
    num_ranges_ = std::exchange(other.num_ranges_, 0);
  }
  return *this;
}

void SymbolizationResultBatch::Free() {
  // c_api_result_.ranges is the base pointer of the single allocation.
  if (c_api_result_.ranges && free_result_fn_) {
    free_result_fn_(c_api_result_);
  }
  c_api_result_ = {};
}

std::pair<const ::LlvmSymbolizedFrame*, uint32_t>
SymbolizationResultBatch::GetFramesForRequest(uint32_t request_index) const {
  if (request_index >= num_ranges_) {
    return {nullptr, 0};
  }
  const auto& range = ranges_ptr_[request_index];
  // Ensure we don't read past the end of the frames buffer.
  if (range.offset + range.num_frames > num_total_frames_) {
    PERFETTO_DFATAL("Invalid range in symbolization result.");
    return {nullptr, 0};
  }
  return {all_frames_ptr_ + range.offset, range.num_frames};
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
    const std::vector<::SymbolizationRequest>& requests) {
  if (!c_api_symbolizer_) {
    return SymbolizationResultBatch({}, free_result_fn_);
  }

  BatchSymbolizationResult batch_result =
      symbolize_fn_(c_api_symbolizer_, requests.data(),
                    static_cast<uint32_t>(requests.size()));
  return SymbolizationResultBatch(batch_result, free_result_fn_);
}

}  // namespace profiling
}  // namespace perfetto
