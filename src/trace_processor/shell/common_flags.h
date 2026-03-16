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

#ifndef SRC_TRACE_PROCESSOR_SHELL_COMMON_FLAGS_H_
#define SRC_TRACE_PROCESSOR_SHELL_COMMON_FLAGS_H_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/metatrace_config.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/shell/subcommand.h"

namespace perfetto::trace_processor {
class TraceProcessorShell_PlatformInterface;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::shell {

// Global options shared by all subcommands.
struct GlobalOptions {
  std::string trace_file;
  bool force_full_sort = false;
  bool no_ftrace_raw = false;
  bool analyze_trace_proto_content = false;
  bool crop_track_events = false;
  bool dev = false;
  std::vector<std::string> dev_flags;
  bool extra_checks = false;
  std::vector<std::string> sql_package_paths;
  std::vector<std::string> override_sql_package_paths;
  std::string override_stdlib_path;
  std::string register_files_dir;
  std::string metatrace_path;
  size_t metatrace_buffer_capacity = 0;
  metatrace::MetatraceCategories metatrace_categories =
      static_cast<metatrace::MetatraceCategories>(
          metatrace::MetatraceCategories::QUERY_TIMELINE |
          metatrace::MetatraceCategories::API_TIMELINE);
};

// Long option IDs for global flags, starting at 1000 to avoid collisions
// with subcommand-specific option IDs (which should start below 1000).
// NOTE: These are used by the classic (legacy) flag parsing path. New
// subcommands use the declarative FlagSpec system via GetGlobalFlagSpecs().
enum GlobalLongOption {
  OPT_GLOBAL_FULL_SORT = 1000,
  OPT_GLOBAL_NO_FTRACE_RAW,
  OPT_GLOBAL_ANALYZE_TRACE_PROTO_CONTENT,
  OPT_GLOBAL_CROP_TRACK_EVENTS,
  OPT_GLOBAL_DEV,
  OPT_GLOBAL_DEV_FLAG,
  OPT_GLOBAL_EXTRA_CHECKS,
  OPT_GLOBAL_ADD_SQL_PACKAGE,
  OPT_GLOBAL_OVERRIDE_SQL_PACKAGE,
  OPT_GLOBAL_OVERRIDE_STDLIB,
  OPT_GLOBAL_REGISTER_FILES_DIR,
  OPT_GLOBAL_METATRACE_BUFFER_CAPACITY,
  OPT_GLOBAL_METATRACE_CATEGORIES,
};

// getopt_long entries for global flags. Used by the classic flag parsing path.
// Does NOT include the terminating {nullptr,0,nullptr,0} sentinel.
// Returns the number of entries via |out_size| if non-null.
const option* GetGlobalLongOptions(size_t* out_size = nullptr);

// Attempts to handle |option| as a global flag, updating |opts|.
// Returns true if handled, false if the option is not a global flag.
// Used only by the classic flag parsing path.
bool HandleGlobalOption(int option, const char* optarg, GlobalOptions* opts);

// Returns FlagSpecs for global options with handlers that modify |*opts|.
// Used by the declarative flag parsing path (ParseFlags).
std::vector<FlagSpec> GetGlobalFlagSpecs(GlobalOptions* opts);

// Parses flags for a subcommand. Combines the subcommand's flags (from
// cmd->GetFlags()) with global flags (modifying *ctx->global). Stores
// remaining positional arguments in ctx->positional_args.
// Sets ctx->help_requested if -h/--help was given.
base::Status ParseFlags(Subcommand* cmd,
                        SubcommandContext* ctx,
                        int argc,
                        char** argv);

// Builds a TraceProcessor Config from global options.
Config BuildConfig(const GlobalOptions& opts,
                   TraceProcessorShell_PlatformInterface* platform);

// Creates a TraceProcessor from the given Config, then applies SQL packages,
// metatrace, and file registration from global options. The caller is free to
// modify |config| between BuildConfig() and this call.
base::StatusOr<std::unique_ptr<TraceProcessor>> SetupTraceProcessor(
    const GlobalOptions& opts,
    const Config& config,
    TraceProcessorShell_PlatformInterface* platform);

// Loads a trace file using the platform interface. Returns the load time.
base::StatusOr<base::TimeNanos> LoadTraceFile(
    TraceProcessor* tp,
    TraceProcessorShell_PlatformInterface* platform,
    const std::string& trace_file);

}  // namespace perfetto::trace_processor::shell

#endif  // SRC_TRACE_PROCESSOR_SHELL_COMMON_FLAGS_H_
