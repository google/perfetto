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

#include "src/profiling/symbolizer/llvm_symbolizer_c_api.h"

namespace {

class LlvmSymbolizerImpl {
 public:
  LlvmSymbolizerImpl();
  ~LlvmSymbolizerImpl();

  BatchSymbolizationResult Symbolize(const SymbolizationRequest* requests,
                                     size_t num_requests);

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
  opts.UseDIA = false;
  opts.PathStyle =
      llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath;
  symbolizer_ = std::make_unique<llvm::symbolize::LLVMSymbolizer>(opts);
}

LlvmSymbolizerImpl::~LlvmSymbolizerImpl() = default;

BatchSymbolizationResult LlvmSymbolizerImpl::Symbolize(
    const SymbolizationRequest* requests,
    size_t num_requests) {
  SymbolizationResult* res = static_cast<SymbolizationResult*>(
      malloc(sizeof(SymbolizationResult) * num_requests));
  if (!res) {
    return {nullptr, 0};
  }

  for (size_t i = 0; i < num_requests; ++i) {
    const auto& request = requests[i];

    // In order to handle the lack of consistency in the signature of
    // symbolizeInlinedCode we will preform checks on LLVM_VERSION_MAJOR (which
    // is defined in llvm/Config/llvm-config.h) in order to correctly call the
    // function.
#if LLVM_VERSION_MAJOR >= 11
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
      res[i] = {nullptr, 0};
      continue;
    }

    const llvm::DIInliningInfo& inlining_info = res_or_err.get();
    uint32_t num_frames = inlining_info.getNumberOfFrames();
    if (num_frames == 0) {
      res[i] = {nullptr, 0};
      continue;
    }

    size_t total_string_size = 0;
    for (uint32_t j = 0; j < num_frames; ++j) {
      const llvm::DILineInfo& line_info = inlining_info.getFrame(j);
      total_string_size += line_info.FunctionName.size() + 1;
      total_string_size += line_info.FileName.size() + 1;
    }

    size_t total_alloc_size =
        (sizeof(SymbolizedFrame) * num_frames) + total_string_size;
    void* buffer = malloc(total_alloc_size);
    if (!buffer) {
      res[i] = {nullptr, 0};
      continue;
    }

    SymbolizedFrame* frames = static_cast<SymbolizedFrame*>(buffer);
    char* string_buffer =
        static_cast<char*>(buffer) + (sizeof(SymbolizedFrame) * num_frames);

    for (uint32_t j = 0; j < num_frames; ++j) {
      const llvm::DILineInfo& line_info = inlining_info.getFrame(j);

      frames[j].function_name = string_buffer;
      memcpy(string_buffer, line_info.FunctionName.c_str(),
             line_info.FunctionName.size() + 1);
      string_buffer += line_info.FunctionName.size() + 1;

      frames[j].file_name = string_buffer;
      memcpy(string_buffer, line_info.FileName.c_str(),
             line_info.FileName.size() + 1);
      string_buffer += line_info.FileName.size() + 1;

      frames[j].line_number = line_info.Line;
    }
    res[i] = {frames, num_frames};
  }
  return {res, num_requests};
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
    size_t num_requests) {
  return reinterpret_cast<LlvmSymbolizerImpl*>(sym)->Symbolize(requests,
                                                               num_requests);
}

void LlvmSymbolizer_FreeBatchSymbolizationResult(
    BatchSymbolizationResult result) {
  for (size_t i = 0; i < result.num_results; ++i) {
    free(result.results[i].frames);
  }
  free(result.results);
}

}  // extern "C"
