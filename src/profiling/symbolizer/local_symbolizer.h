/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_PROFILING_SYMBOLIZER_LOCAL_SYMBOLIZER_H_
#define SRC_PROFILING_SYMBOLIZER_LOCAL_SYMBOLIZER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/pipe.h"
#include "src/profiling/symbolizer/symbolizer.h"

namespace perfetto {
namespace profiling {

#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)

bool ParseLlvmSymbolizerLine(const std::string& line,
                             std::string* file_name,
                             uint32_t* line_no);

class BinaryFinder {
 public:
  virtual ~BinaryFinder();
  virtual base::Optional<std::string> FindBinary(
      const std::string& abspath,
      const std::string& build_id) = 0;
};

class LocalBinaryIndexer : public BinaryFinder {
 public:
  explicit LocalBinaryIndexer(std::vector<std::string> roots);

  base::Optional<std::string> FindBinary(const std::string& abspath,
                                         const std::string& build_id) override;
  ~LocalBinaryIndexer() override;

 private:
  std::map<std::string, std::string> buildid_to_file_;
};

class LocalBinaryFinder : public BinaryFinder {
 public:
  explicit LocalBinaryFinder(std::vector<std::string> roots);

  base::Optional<std::string> FindBinary(const std::string& abspath,
                                         const std::string& build_id) override;

  ~LocalBinaryFinder() override;

 private:
  bool IsCorrectFile(const std::string& symbol_file,
                     const std::string& build_id);

  base::Optional<std::string> FindBinaryInRoot(const std::string& root_str,
                                               const std::string& abspath,
                                               const std::string& build_id);

 private:
  std::vector<std::string> roots_;
  std::map<std::string, base::Optional<std::string>> cache_;
};

class Subprocess {
 public:
  Subprocess(const std::string& file, std::vector<std::string> args);

  ~Subprocess();

  int read_fd() { return output_pipe_.rd.get(); }
  int write_fd() { return input_pipe_.wr.get(); }

 private:
  base::Pipe input_pipe_;
  base::Pipe output_pipe_;

  pid_t pid_ = -1;
};

class LLVMSymbolizerProcess {
 public:
  LLVMSymbolizerProcess();

  std::vector<SymbolizedFrame> Symbolize(const std::string& binary,
                                         uint64_t address);

 private:
  Subprocess subprocess_;
  FILE* read_file_;
};

class LocalSymbolizer : public Symbolizer {
 public:
  explicit LocalSymbolizer(std::unique_ptr<BinaryFinder> finder)
      : finder_(std::move(finder)) {}

  std::vector<std::vector<SymbolizedFrame>> Symbolize(
      const std::string& mapping_name,
      const std::string& build_id,
      const std::vector<uint64_t>& address) override;

  ~LocalSymbolizer() override;

 private:
  LLVMSymbolizerProcess llvm_symbolizer_;
  std::unique_ptr<BinaryFinder> finder_;
};

#endif  // PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)

std::unique_ptr<Symbolizer> LocalSymbolizerOrDie(
    std::vector<std::string> binary_path,
    const char* mode);

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_SYMBOLIZER_LOCAL_SYMBOLIZER_H_
