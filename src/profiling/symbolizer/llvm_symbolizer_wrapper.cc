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

  SymbolizationResult Symbolize(const char* binary_path, uint64_t address);

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

SymbolizationResult LlvmSymbolizerImpl::Symbolize(const char* binary_path,
                                                  uint64_t address) {
#if LLVM_VERSION_MAJOR >= 11
  llvm::Expected<llvm::DIInliningInfo> res_or_err =
      symbolizer_->symbolizeInlinedCode(
          binary_path, {address, llvm::object::SectionedAddress::UndefSection});
#else
  llvm::Expected<llvm::DIInliningInfo> res_or_err =
      symbolizer_->symbolizeInlinedCode(binary_path, address);
#endif

  if (!res_or_err) {
    std::string err_msg;
    llvm::raw_string_ostream os(err_msg);
    llvm::logAllUnhandledErrors(res_or_err.takeError(), os,
                                "LLVM Symbolizer error: ");
    fprintf(stderr,
            "Perfetto-LLVM-Wrapper: Failed to symbolize 0x%" PRIx64
            " in %s: %s\n",
            address, binary_path, os.str().c_str());
    return {nullptr, 0};
  }

  const llvm::DIInliningInfo& inlining_info = res_or_err.get();
  uint32_t num_frames = inlining_info.getNumberOfFrames();
  if (num_frames == 0) {
    return {nullptr, 0};
  }

  size_t total_string_size = 0;
  for (uint32_t i = 0; i < num_frames; ++i) {
    const llvm::DILineInfo& line_info = inlining_info.getFrame(i);
    total_string_size += line_info.FunctionName.size() + 1;
    total_string_size += line_info.FileName.size() + 1;
  }

  size_t total_alloc_size =
      (sizeof(SymbolizedFrame) * num_frames) + total_string_size;
  void* buffer = malloc(total_alloc_size);
  if (!buffer) {
    return {nullptr, 0};
  }

  SymbolizedFrame* frames = static_cast<SymbolizedFrame*>(buffer);
  char* string_buffer =
      static_cast<char*>(buffer) + (sizeof(SymbolizedFrame) * num_frames);

  for (uint32_t i = 0; i < num_frames; ++i) {
    const llvm::DILineInfo& line_info = inlining_info.getFrame(i);

    frames[i].function_name = string_buffer;
    memcpy(string_buffer, line_info.FunctionName.c_str(),
           line_info.FunctionName.size() + 1);
    string_buffer += line_info.FunctionName.size() + 1;

    frames[i].file_name = string_buffer;
    memcpy(string_buffer, line_info.FileName.c_str(),
           line_info.FileName.size() + 1);
    string_buffer += line_info.FileName.size() + 1;

    frames[i].line_number = line_info.Line;
  }

  return {frames, num_frames};
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

SymbolizationResult LlvmSymbolizer_Symbolize(LlvmSymbolizer* sym,
                                             const char* binary_path,
                                             uint64_t address) {
  return reinterpret_cast<LlvmSymbolizerImpl*>(sym)->Symbolize(binary_path,
                                                               address);
}

void LlvmSymbolizer_FreeSymbolizationResult(SymbolizationResult result) {
  free(result.frames);
}

}  // extern "C"
