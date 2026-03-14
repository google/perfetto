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

#include "src/trace_processor/shell/subcommand.h"

#include <cstring>
#include <unordered_set>

#include "perfetto/ext/base/getopt.h"

namespace perfetto::trace_processor::shell {

Subcommand::~Subcommand() = default;

FindSubcommandResult FindSubcommandInArgs(
    int argc,
    char** argv,
    const std::vector<Subcommand*>& subcommands,
    const std::vector<Subcommand*>& all_subcommands) {
  // Build the set of long flags that consume the next argument, derived
  // from the getopt_long options of all registered subcommands.
  std::unordered_set<std::string> flags_with_arg;
  for (const auto* sc : all_subcommands) {
    for (const option* o = sc->GetLongOptions(); o && o->name; ++o) {
      if (o->has_arg == required_argument) {
        flags_with_arg.insert("--" + std::string(o->name));
      }
    }
  }

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];

    // Skip flags.
    if (arg[0] == '-') {
      // Check if this flag consumes the next argument.
      if (flags_with_arg.count(arg)) {
        ++i;  // Skip the flag's argument.
      }
      continue;
    }

    // Positional argument: check if it matches a subcommand.
    for (auto* sc : subcommands) {
      if (strcmp(sc->name(), arg) == 0) {
        return {sc, i};
      }
    }

    // Unknown positional argument (likely a trace file) — stop searching.
    break;
  }
  return {};
}

}  // namespace perfetto::trace_processor::shell
