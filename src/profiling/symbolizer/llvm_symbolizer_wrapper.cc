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

#include <llvm/Config/llvm-config.h>
#include <llvm/DebugInfo/DIContext.h>
#include <llvm/DebugInfo/Symbolize/Symbolize.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <new>
#include <string>
#include <vector>

#include "src/profiling/symbolizer/llvm_symbolizer_c_api.h"

namespace {

class LlvmSymbolizerImpl {
 public:
  LlvmSymbolizerImpl();
  ~LlvmSymbolizerImpl();

  BatchSymbolizationResult Symbolize(const SymbolizationRequest* requests,
                                     uint32_t num_requests);

 private:
  std::unique_ptr<llvm::symbolize::LLVMSymbolizer> symbolizer_;
};

LlvmSymbolizerImpl::LlvmSymbolizerImpl() {
  llvm::symbolize::LLVMSymbolizer::Options opts;
  opts.UseSymbolTable = true;
  opts.Demangle = true;
  opts.PrintFunctions = llvm::symbolize::FunctionNameKind::LinkageName;
  opts.RelativeAddresses = false;
  opts.UntagAddresses = true;
#if LLVM_VERSION_MAJOR >= 12
  opts.UseDIA = false;
#endif
#if LLVM_VERSION_MAJOR >= 11
  opts.PathStyle =
      llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath;
#endif
  symbolizer_ = std::make_unique<llvm::symbolize::LLVMSymbolizer>(opts);
}

LlvmSymbolizerImpl::~LlvmSymbolizerImpl() = default;

BatchSymbolizationResult LlvmSymbolizerImpl::Symbolize(
    const SymbolizationRequest* requests,
    uint32_t num_requests) {
  std::vector<llvm::DIInliningInfo> llvm_results;
  llvm_results.reserve(num_requests);

  uint32_t total_frames = 0;
  size_t total_string_size = 0;

  // --- First Pass: Symbolize all requests and calculate total memory size ---
  // The expensive LLVM symbolization happens only in this loop.
  for (uint32_t i = 0; i < num_requests; ++i) {
    const auto& request = requests[i];

#if LLVM_VERSION_MAJOR >= 9
    llvm::Expected<llvm::DIInliningInfo> res_or_err =
        symbolizer_->symbolizeInlinedCode(
            request.binary_path,
            {request.address, llvm::object::SectionedAddress::UndefSection});
#else
    llvm::Expected<llvm::DIInliningInfo> res_or_err =
        symbolizer_->symbolizeInlinedCode(request.binary_path, request.address);
#endif

    if (!res_or_err) {
      std::string err_msg;
      llvm::raw_string_ostream os(err_msg);
      llvm::logAllUnhandledErrors(res_or_err.takeError(), os,
                                  "LLVM Symbolizer error: ");
      fprintf(stderr,
              "Perfetto-LLVM-Wrapper: Failed to symbolize 0x%" PRIx64
              " in %s: %s\n",
              request.address, request.binary_path, os.str().c_str());
      llvm_results.emplace_back();  // Add empty result for failed ones.
      continue;
    }

    llvm::DIInliningInfo inlining_info = std::move(res_or_err.get());

    uint32_t num_frames = inlining_info.getNumberOfFrames();
    total_frames += num_frames;
    for (uint32_t j = 0; j < num_frames; ++j) {
      const llvm::DILineInfo& line_info = inlining_info.getFrame(j);
      total_string_size += line_info.FunctionName.size() + 1;
      total_string_size += line_info.FileName.size() + 1;
    }

    llvm_results.push_back(std::move(inlining_info));
  }

  size_t ranges_size =
      sizeof(SymbolizationResultRange) * static_cast<size_t>(num_requests);
  size_t frames_size =
      sizeof(SymbolizedFrame) * static_cast<size_t>(total_frames);
  size_t total_alloc_size = ranges_size + frames_size + total_string_size;

  if (total_alloc_size == 0) {
    return {nullptr, 0, nullptr, 0};
  }

  void* buffer = malloc(total_alloc_size);
  if (!buffer) {
    return {nullptr, 0, nullptr, 0};
  }

  // Carve up the single buffer into sections for ranges, frames, and strings.
  SymbolizationResultRange* ranges_ptr =
      static_cast<SymbolizationResultRange*>(buffer);
  SymbolizedFrame* frames_ptr = reinterpret_cast<SymbolizedFrame*>(
      ranges_ptr + static_cast<size_t>(num_requests));
  char* string_ptr =
      reinterpret_cast<char*>(frames_ptr + static_cast<size_t>(total_frames));

  uint32_t current_frame_offset = 0;
  for (uint32_t i = 0; i < num_requests; ++i) {
    const auto& inlining_info = llvm_results[i];
    uint32_t num_frames = inlining_info.getNumberOfFrames();

    ranges_ptr[i] = {current_frame_offset, num_frames};

    for (uint32_t j = 0; j < num_frames; ++j) {
      const llvm::DILineInfo& line_info = inlining_info.getFrame(j);
      SymbolizedFrame& frame = frames_ptr[current_frame_offset + j];

      frame.function_name = string_ptr;
      memcpy(string_ptr, line_info.FunctionName.c_str(),
             line_info.FunctionName.size() + 1);
      string_ptr += line_info.FunctionName.size() + 1;

      frame.file_name = string_ptr;
      memcpy(string_ptr, line_info.FileName.c_str(),
             line_info.FileName.size() + 1);
      string_ptr += line_info.FileName.size() + 1;

      frame.line_number = line_info.Line;
    }
    current_frame_offset += num_frames;
  }

  return {frames_ptr, total_frames, ranges_ptr, num_requests};
}

}  // namespace

// C API Implementation
extern "C" {

LlvmSymbolizer* LlvmSymbolizer_Create() {
  return reinterpret_cast<LlvmSymbolizer*>(new (std::nothrow)
                                               LlvmSymbolizerImpl());
}

void LlvmSymbolizer_Destroy(LlvmSymbolizer* sym) {
  delete reinterpret_cast<LlvmSymbolizerImpl*>(sym);
}

BatchSymbolizationResult LlvmSymbolizer_Symbolize(
    LlvmSymbolizer* sym,
    const SymbolizationRequest* requests,
    uint32_t num_requests) {
  return reinterpret_cast<LlvmSymbolizerImpl*>(sym)->Symbolize(requests,
                                                               num_requests);
}

void LlvmSymbolizer_FreeBatchSymbolizationResult(
    BatchSymbolizationResult result) {
  free(result.ranges);
}

}  // extern "C"
