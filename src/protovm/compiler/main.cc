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

#include <cstdio>
#include <string_view>

#include "perfetto/ext/base/file_utils.h"
#include "src/protovm/compiler/compiler.h"

int ProcessArgs(int argc, char** argv);

// Implements the ProtoVM compiler CLI
//
// Reads a CompileConfig textproto from stdin and outputs the compiled binary
// VmProgram to stdout.
int main(int argc, char** argv) {
  if (argc > 1) {
    ProcessArgs(argc, argv);
  }

  std::string textproto;
  if (!perfetto::base::ReadFileStream(stdin, &textproto)) {
    PERFETTO_ELOG("Failed to read from stdin");
    return 1;
  }

  auto compiler = perfetto::protovm::Compiler{};
  auto status_or_program = compiler.Compile(textproto);
  if (!status_or_program.ok()) {
    PERFETTO_ELOG("Encountered error: %s",
                  status_or_program.status().c_message());
    return 1;
  }

  std::fwrite(status_or_program->data(), 1, status_or_program->size(), stdout);

  return 0;
}

int ProcessArgs(int argc, char** argv) {
  if (argc == 1) {
    std::string_view arg{argv[1]};
    if (arg == "--help" || arg == "-h") {
      std::printf(
          "Usage: %s\n"
          "Reads ProtoVM CompileConfig textproto from stdin, compiles it and "
          "outputs a VmProgram binary on stdout.\n",
          argv[0]);
      return 0;
    }
  }
  std::fprintf(stderr, "Unknown arguments\n");
  return 1;
}
