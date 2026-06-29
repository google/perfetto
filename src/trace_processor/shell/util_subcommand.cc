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

#include "src/trace_processor/shell/util_subcommand.h"

#include <fstream>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/shell/convert_helpers.h"
#include "src/trace_processor/shell/subcommand.h"
#include "src/traceconv/deobfuscate_profile.h"
#include "src/traceconv/symbolize_profile.h"
#include "src/traceconv/trace_unpack.h"

namespace perfetto::trace_processor::shell {

const char* UtilSubcommand::name() const {
  return "util";
}

const char* UtilSubcommand::description() const {
  return "Low-level trace utilities (symbolize, deobfuscate, etc.).";
}

const char* UtilSubcommand::usage_args() const {
  return "<symbolize|deobfuscate|decompress_packets|text_to_binary> [input] "
         "[output]";
}

const char* UtilSubcommand::detailed_help() const {
  return R"(Low-level trace utilities.

Utilities:
  symbolize            Symbolize addresses in a profile, emitting symbol packets.
  deobfuscate          Emit deobfuscation packets from a trace.
  decompress_packets   Decompress compressed trace packets.
  text_to_binary       Convert a text-format trace proto to binary.

If no input file is given, reads from stdin.
If no output file is given, writes to stdout.

symbolize/deobfuscate are lower-level than 'bundle', which is the recommended
one-shot way to produce a self-contained, symbolized trace.)";
}

std::vector<FlagSpec> UtilSubcommand::GetFlags() {
  return {
      BoolFlag("verbose", '\0', "Print more detailed output.", &verbose_),
  };
}

base::Status UtilSubcommand::Run(const SubcommandContext& ctx) {
  if (ctx.positional_args.empty()) {
    return base::ErrStatus(
        "util: a utility ('symbolize' or 'deobfuscate') must be specified.");
  }
  const std::string& util = ctx.positional_args[0];
  const std::string input_path =
      ctx.positional_args.size() > 1 ? ctx.positional_args[1] : "";
  const std::string output_path =
      ctx.positional_args.size() > 2 ? ctx.positional_args[2] : "";

  if (util != "symbolize" && util != "deobfuscate" &&
      util != "decompress_packets" && util != "text_to_binary") {
    return base::ErrStatus(
        "util: unknown utility '%s' (expected 'symbolize', 'deobfuscate', "
        "'decompress_packets' or 'text_to_binary').",
        util.c_str());
  }

  std::ifstream input_file;
  std::istream* input = nullptr;
  RETURN_IF_ERROR(OpenConversionInput(input_path, &input_file, &input));

  std::ofstream output_file;
  std::ostream* output = nullptr;
  RETURN_IF_ERROR(OpenConversionOutput(output_path, &output_file, &output));

  int ret;
  if (util == "symbolize") {
    ret = trace_to_text::SymbolizeProfile(input, output, verbose_);
  } else if (util == "deobfuscate") {
    ret = trace_to_text::DeobfuscateProfile(input, output);
  } else if (util == "decompress_packets") {
    ret = trace_to_text::UnpackCompressedPackets(input, output) ? 0 : 1;
  } else {  // text_to_binary
    ret = TextToTrace(input, output);
  }
  if (ret != 0)
    return base::ErrStatus("util: '%s' failed.", util.c_str());
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::shell
