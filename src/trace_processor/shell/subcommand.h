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

#ifndef SRC_TRACE_PROCESSOR_SHELL_SUBCOMMAND_H_
#define SRC_TRACE_PROCESSOR_SHELL_SUBCOMMAND_H_

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "perfetto/base/status.h"

namespace perfetto::trace_processor {
class TraceProcessorShell_PlatformInterface;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::shell {

struct GlobalOptions;

// Context passed to subcommands, providing access to shared resources.
struct SubcommandContext {
  TraceProcessorShell_PlatformInterface* platform = nullptr;

  // Parsed global options. Set by ParseFlags() before Run() is called.
  GlobalOptions* global = nullptr;

  // Positional arguments remaining after flag parsing (e.g., trace file path).
  std::vector<std::string> positional_args;

  // Set by ParseFlags() when -h/--help is given. When true, Run() should not
  // be called (usage has already been printed).
  bool help_requested = false;
};

// Specifies a single command-line flag for a subcommand.
struct FlagSpec {
  const char* long_name;
  char short_name = 0;
  bool has_arg = false;
  const char* arg_name = "";
  const char* help = "";
  std::function<void(const char*)> handler;
};

// Helper: creates a FlagSpec that stores the argument into a std::string.
inline FlagSpec StringFlag(const char* long_name,
                           char short_name,
                           const char* arg_name,
                           const char* help,
                           std::string* target) {
  return {
      long_name, short_name, true,
      arg_name,  help,       [target](const char* a) { *target = a; },
  };
}

// Helper: creates a FlagSpec that sets a bool to true.
inline FlagSpec BoolFlag(const char* long_name,
                         char short_name,
                         const char* help,
                         bool* target) {
  return {
      long_name, short_name, false,
      "",        help,       [target](const char*) { *target = true; },
  };
}

// Base class for all subcommands (query, export, serve, etc.).
class Subcommand {
 public:
  virtual ~Subcommand();

  // The name of the subcommand as it appears on the command line
  // (e.g. "query", "export").
  virtual const char* name() const = 0;

  // A short one-line description shown in help output.
  virtual const char* description() const = 0;

  // Returns the flags this subcommand accepts. The returned FlagSpecs
  // typically have handlers that capture references to member variables.
  virtual std::vector<FlagSpec> GetFlags() = 0;

  // Runs the subcommand after flags have been parsed. |ctx| provides
  // access to the platform interface, parsed global options, and
  // positional arguments.
  virtual base::Status Run(const SubcommandContext& ctx) = 0;
};

// Result of FindSubcommandInArgs().
struct FindSubcommandResult {
  Subcommand* subcommand = nullptr;
  int argv_index = -1;
};

// Searches |argv[1..argc-1]| for the first positional argument that matches
// a registered subcommand name. Skips flags (arguments starting with '-') and
// their required arguments (those listed in |flags_with_arg|, e.g., "--file"
// or "-f").
FindSubcommandResult FindSubcommandInArgs(
    int argc,
    char** argv,
    const std::vector<Subcommand*>& subcommands,
    const std::unordered_set<std::string>& flags_with_arg);

}  // namespace perfetto::trace_processor::shell

#endif  // SRC_TRACE_PROCESSOR_SHELL_SUBCOMMAND_H_
