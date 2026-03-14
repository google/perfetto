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

#include <string>
#include <vector>

struct option;  // Forward declaration for getopt.h's struct option.

namespace perfetto::trace_processor {
class TraceProcessorShell_PlatformInterface;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::shell {

// Context passed to subcommands, providing access to shared resources.
struct SubcommandContext {
  TraceProcessorShell_PlatformInterface* platform = nullptr;
};

// Base class for all subcommands (query, export, serve, etc.).
class Subcommand {
 public:
  virtual ~Subcommand();

  // The name of the subcommand as it appears on the command line
  // (e.g. "query", "export").
  virtual const char* name() const = 0;

  // A short one-line description shown in help output.
  virtual const char* description() const = 0;

  // Runs the subcommand. |ctx| provides access to shared resources like the
  // platform interface. |argc| and |argv| are the original command line with
  // the subcommand name removed (argv[0] is the program name).
  // Returns 0 on success, non-zero on failure.
  virtual int Run(const SubcommandContext& ctx, int argc, char** argv) = 0;

  // Prints subcommand-specific usage to stderr.
  virtual void PrintUsage(const char* argv0) = 0;

  // Returns the null-terminated getopt_long options array for this subcommand.
  // Used by FindSubcommandInArgs to determine which flags consume an argument.
  virtual const option* GetLongOptions() const = 0;
};

// Result of FindSubcommandInArgs(). If |subcommand| is non-null, a subcommand
// was found. |argv_index| is the index of the subcommand name in the original
// argv.
struct FindSubcommandResult {
  Subcommand* subcommand = nullptr;
  int argv_index = -1;
};

// Searches |argv[1..argc-1]| for the first positional argument that matches
// a registered subcommand name. Skips flags (arguments starting with '-') and
// their required arguments (derived from the long options of all subcommands
// in |all_subcommands|).
//
// |subcommands| is the list of subcommands to match against.
// |all_subcommands| is the full list including those not matched (e.g. classic)
// whose flags still need to be skipped during scanning.
FindSubcommandResult FindSubcommandInArgs(
    int argc,
    char** argv,
    const std::vector<Subcommand*>& subcommands,
    const std::vector<Subcommand*>& all_subcommands);

}  // namespace perfetto::trace_processor::shell

#endif  // SRC_TRACE_PROCESSOR_SHELL_SUBCOMMAND_H_
