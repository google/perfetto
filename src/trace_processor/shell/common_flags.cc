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

#include "src/trace_processor/shell/common_flags.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/trace_processor/trace_processor_shell.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/metatrace_config.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/shell/metatrace.h"
#include "src/trace_processor/shell/shell_utils.h"
#include "src/trace_processor/shell/sql_packages.h"
#include "src/trace_processor/shell/subcommand.h"
#include "src/trace_processor/util/deobfuscation/deobfuscator.h"
#include "src/trace_processor/util/symbolizer/symbolize_database.h"

namespace perfetto::trace_processor::shell {

const option* GetGlobalLongOptions(size_t* out_size) {
  static const option kGlobalLongOptions[] = {
      {"full-sort", no_argument, nullptr, OPT_GLOBAL_FULL_SORT},
      {"no-ftrace-raw", no_argument, nullptr, OPT_GLOBAL_NO_FTRACE_RAW},
      {"analyze-trace-proto-content", no_argument, nullptr,
       OPT_GLOBAL_ANALYZE_TRACE_PROTO_CONTENT},
      {"crop-track-events", no_argument, nullptr, OPT_GLOBAL_CROP_TRACK_EVENTS},
      {"dev", no_argument, nullptr, OPT_GLOBAL_DEV},
      {"dev-flag", required_argument, nullptr, OPT_GLOBAL_DEV_FLAG},
      {"extra-checks", no_argument, nullptr, OPT_GLOBAL_EXTRA_CHECKS},
      {"add-sql-package", required_argument, nullptr,
       OPT_GLOBAL_ADD_SQL_PACKAGE},
      {"override-sql-package", required_argument, nullptr,
       OPT_GLOBAL_OVERRIDE_SQL_PACKAGE},
      {"override-stdlib", required_argument, nullptr,
       OPT_GLOBAL_OVERRIDE_STDLIB},
      {"register-files-dir", required_argument, nullptr,
       OPT_GLOBAL_REGISTER_FILES_DIR},
      {"metatrace", required_argument, nullptr, 'm'},
      {"metatrace-buffer-capacity", required_argument, nullptr,
       OPT_GLOBAL_METATRACE_BUFFER_CAPACITY},
      {"metatrace-categories", required_argument, nullptr,
       OPT_GLOBAL_METATRACE_CATEGORIES},
  };
  if (out_size)
    *out_size = base::ArraySize(kGlobalLongOptions);
  return kGlobalLongOptions;
}

bool HandleGlobalOption(int option, const char* optarg, GlobalOptions* opts) {
  switch (option) {
    case 'm':
      opts->metatrace_path = optarg;
      return true;
    case OPT_GLOBAL_FULL_SORT:
      opts->force_full_sort = true;
      return true;
    case OPT_GLOBAL_NO_FTRACE_RAW:
      opts->no_ftrace_raw = true;
      return true;
    case OPT_GLOBAL_ANALYZE_TRACE_PROTO_CONTENT:
      opts->analyze_trace_proto_content = true;
      return true;
    case OPT_GLOBAL_CROP_TRACK_EVENTS:
      opts->crop_track_events = true;
      return true;
    case OPT_GLOBAL_DEV:
      opts->dev = true;
      return true;
    case OPT_GLOBAL_DEV_FLAG:
      opts->dev_flags.emplace_back(optarg);
      return true;
    case OPT_GLOBAL_EXTRA_CHECKS:
      opts->extra_checks = true;
      return true;
    case OPT_GLOBAL_ADD_SQL_PACKAGE:
      opts->sql_package_paths.emplace_back(optarg);
      return true;
    case OPT_GLOBAL_OVERRIDE_SQL_PACKAGE:
      opts->override_sql_package_paths.emplace_back(optarg);
      return true;
    case OPT_GLOBAL_OVERRIDE_STDLIB:
      opts->override_stdlib_path = optarg;
      return true;
    case OPT_GLOBAL_REGISTER_FILES_DIR:
      opts->register_files_dir = optarg;
      return true;
    case OPT_GLOBAL_METATRACE_BUFFER_CAPACITY:
      opts->metatrace_buffer_capacity = static_cast<size_t>(atoi(optarg));
      return true;
    case OPT_GLOBAL_METATRACE_CATEGORIES:
      opts->metatrace_categories = ParseMetatraceCategories(optarg);
      return true;
    default:
      return false;
  }
}

std::vector<FlagSpec> GetGlobalFlagSpecs(GlobalOptions* opts) {
  return {
      BoolFlag("full-sort", 0, "Force full sort.", &opts->force_full_sort),
      BoolFlag("no-ftrace-raw", 0, "Skip typed ftrace in raw table.",
               &opts->no_ftrace_raw),
      BoolFlag("analyze-trace-proto-content", 0,
               "Enable trace proto content analysis.",
               &opts->analyze_trace_proto_content),
      BoolFlag("crop-track-events", 0,
               "Ignore track events outside range of interest.",
               &opts->crop_track_events),
      BoolFlag("dev", 0, "Enable development-only features.", &opts->dev),
      {
          "dev-flag",
          0,
          true,
          "KEY=VALUE",
          "Set a dev flag (requires --dev).",
          [opts](const char* a) { opts->dev_flags.emplace_back(a); },
      },
      BoolFlag("extra-checks", 0, "Enable additional SQL validation.",
               &opts->extra_checks),
      {
          "add-sql-package",
          0,
          true,
          "PATH[@PKG]",
          "Register SQL package directory.",
          [opts](const char* a) { opts->sql_package_paths.emplace_back(a); },
      },
      {
          "override-sql-package",
          0,
          true,
          "PATH[@PKG]",
          "Override existing SQL package.",
          [opts](const char* a) {
            opts->override_sql_package_paths.emplace_back(a);
          },
      },
      StringFlag("override-stdlib", 0, "PATH",
                 "Override stdlib (requires --dev).",
                 &opts->override_stdlib_path),
      StringFlag("register-files-dir", 0, "PATH",
                 "Directory with files for importers.",
                 &opts->register_files_dir),
      StringFlag("metatrace", 'm', "FILE", "Write TP metatrace to FILE.",
                 &opts->metatrace_path),
      {
          "metatrace-buffer-capacity",
          0,
          true,
          "N",
          "Metatrace event buffer size.",
          [opts](const char* a) {
            opts->metatrace_buffer_capacity = static_cast<size_t>(atoi(a));
          },
      },
      {
          "metatrace-categories",
          0,
          true,
          "CATS",
          "Comma-separated metatrace categories.",
          [opts](const char* a) {
            opts->metatrace_categories = ParseMetatraceCategories(a);
          },
      },
  };
}

namespace {

void PrintSubcommandUsage(Subcommand* cmd,
                          const std::vector<FlagSpec>& subcmd_flags,
                          const std::vector<FlagSpec>& global_flags,
                          const char* argv0) {
  fprintf(stderr, "\n%s\n\n", cmd->description());
  fprintf(stderr, "Usage: %s %s [flags] trace_file\n\n", argv0, cmd->name());

  auto print_flags = [](const char* heading,
                        const std::vector<FlagSpec>& flags) {
    if (flags.empty())
      return;
    fprintf(stderr, "%s:\n", heading);
    for (const auto& f : flags) {
      std::string lhs = "  ";
      if (f.short_name) {
        lhs += '-';
        lhs += f.short_name;
        lhs += ", ";
      } else {
        lhs += "    ";
      }
      lhs += "--";
      lhs += f.long_name;
      if (f.has_arg && f.arg_name[0]) {
        lhs += ' ';
        lhs += f.arg_name;
      }
      constexpr size_t kPadTo = 36;
      if (lhs.size() < kPadTo) {
        lhs.resize(kPadTo, ' ');
      } else {
        lhs += "  ";
      }
      fprintf(stderr, "%s%s\n", lhs.c_str(), f.help);
    }
    fprintf(stderr, "\n");
  };

  print_flags("Flags", subcmd_flags);
  print_flags("Global flags", global_flags);
}

}  // namespace

base::Status ParseFlags(Subcommand* cmd,
                        SubcommandContext* ctx,
                        int argc,
                        char** argv) {
  auto subcmd_flags = cmd->GetFlags();
  auto global_flags = GetGlobalFlagSpecs(ctx->global);

  // Build the getopt_long option array and handler map.
  std::unordered_map<int, std::function<void(const char*)>> handlers;
  std::vector<option> opts;
  std::string short_opts = "h";
  int next_id = 256;

  auto register_flag = [&](const FlagSpec& f) {
    int id;
    if (f.short_name != 0) {
      id = static_cast<int>(static_cast<unsigned char>(f.short_name));
      short_opts += f.short_name;
      if (f.has_arg)
        short_opts += ':';
    } else {
      id = next_id++;
    }
    option o;
    o.name = f.long_name;
    o.has_arg = f.has_arg ? required_argument : no_argument;
    o.flag = nullptr;
    o.val = id;
    opts.push_back(o);
    handlers[id] = f.handler;
  };

  for (const auto& f : subcmd_flags)
    register_flag(f);
  for (const auto& f : global_flags)
    register_flag(f);

  // Add --help as a long option.
  opts.push_back({"help", no_argument, nullptr, 'h'});
  // Sentinel.
  opts.push_back({nullptr, 0, nullptr, 0});

  // Parse.
  optind = 1;
  for (;;) {
    int opt = getopt_long(argc, argv, short_opts.c_str(), opts.data(), nullptr);
    if (opt == -1)
      break;

    if (opt == 'h') {
      PrintSubcommandUsage(cmd, subcmd_flags, global_flags, argv[0]);
      ctx->help_requested = true;
      return base::OkStatus();
    }

    auto it = handlers.find(opt);
    if (it != handlers.end()) {
      it->second(optarg);
      continue;
    }

    // Unknown option (getopt already printed an error to stderr).
    return base::ErrStatus("Run '%s %s --help' for usage.", argv[0],
                           cmd->name());
  }

  // Collect positional arguments.
  for (int i = optind; i < argc; ++i) {
    ctx->positional_args.emplace_back(argv[i]);
  }

  return base::OkStatus();
}

Config BuildConfig(const GlobalOptions& opts,
                   TraceProcessorShell_PlatformInterface* platform) {
  Config config = platform->DefaultConfig();
  config.sorting_mode = opts.force_full_sort ? SortingMode::kForceFullSort
                                             : SortingMode::kDefaultHeuristics;
  config.ingest_ftrace_in_raw_table = !opts.no_ftrace_raw;
  config.analyze_trace_proto_content = opts.analyze_trace_proto_content;
  config.drop_track_event_data_before =
      opts.crop_track_events
          ? DropTrackEventDataBefore::kTrackEventRangeOfInterest
          : DropTrackEventDataBefore::kNoDrop;
  if (opts.dev) {
    config.enable_dev_features = true;
    for (const auto& flag_pair : opts.dev_flags) {
      auto kv = base::SplitString(flag_pair, "=");
      if (kv.size() != 2) {
        PERFETTO_ELOG("Ignoring unknown dev flag format %s", flag_pair.c_str());
        continue;
      }
      config.dev_flags.emplace(kv[0], kv[1]);
    }
  }
  if (opts.extra_checks) {
    config.enable_extra_checks = true;
  }
  return config;
}

base::StatusOr<std::unique_ptr<TraceProcessor>> SetupTraceProcessor(
    const GlobalOptions& opts,
    const Config& config,
    TraceProcessorShell_PlatformInterface* platform) {
  auto tp = TraceProcessor::CreateInstance(config);
  RETURN_IF_ERROR(platform->OnTraceProcessorCreated(tp.get()));

  // SQL packages.
  if (!opts.override_stdlib_path.empty()) {
    if (!opts.dev) {
      return base::ErrStatus("Overriding stdlib requires --dev flag");
    }
    RETURN_IF_ERROR(LoadOverridenStdlib(tp.get(), opts.override_stdlib_path));
  }
  for (const auto& path : opts.override_sql_package_paths) {
    RETURN_IF_ERROR(IncludeSqlPackage(tp.get(), path, true));
  }
  for (const auto& path : opts.sql_package_paths) {
    RETURN_IF_ERROR(IncludeSqlPackage(tp.get(), path, false));
  }

  // Metatrace.
  if (!opts.metatrace_path.empty()) {
    metatrace::MetatraceConfig metatrace_config;
    metatrace_config.override_buffer_size = opts.metatrace_buffer_capacity;
    metatrace_config.categories = opts.metatrace_categories;
    tp->EnableMetatrace(metatrace_config);
  }

  // Register files.
  if (!opts.register_files_dir.empty()) {
    RETURN_IF_ERROR(RegisterAllFilesInFolder(opts.register_files_dir, *tp));
  }

  return std::move(tp);
}

base::StatusOr<base::TimeNanos> LoadTraceFile(
    TraceProcessor* tp,
    TraceProcessorShell_PlatformInterface* platform,
    const std::string& trace_file) {
  base::TimeNanos t_load_start = base::GetWallTimeNs();
  double size_mb = 0;
  base::Status status =
      platform->LoadTrace(tp, trace_file, [&size_mb](size_t parsed_size) {
        size_mb = static_cast<double>(parsed_size) / 1E6;
        fprintf(stderr, "\rLoading trace: %.2f MB\r", size_mb);
      });
  if (!status.ok()) {
    return base::ErrStatus("Could not read trace file (path: %s): %s",
                           trace_file.c_str(), status.c_message());
  }

  // Symbolize and deobfuscate before finalizing the trace.
  bool is_proto_trace = false;
  {
    auto it = tp->ExecuteQuery(
        "SELECT str_value FROM metadata WHERE name = 'trace_type'");
    while (it.Next()) {
      if (it.Get(0).type == SqlValue::kString &&
          std::string_view(it.Get(0).AsString()) == "proto") {
        is_proto_trace = true;
        break;
      }
    }
  }

  profiling::SymbolizerConfig sym_config;
  const char* mode = getenv("PERFETTO_SYMBOLIZER_MODE");
  std::vector<std::string> paths = profiling::GetPerfettoBinaryPath();
  if (mode && std::string_view(mode) == "find") {
    sym_config.find_symbol_paths = std::move(paths);
  } else {
    sym_config.index_symbol_paths = std::move(paths);
  }
  if (!sym_config.index_symbol_paths.empty() ||
      !sym_config.find_symbol_paths.empty()) {
    if (is_proto_trace) {
      tp->Flush();
      auto sym_result =
          profiling::SymbolizeDatabaseAndLog(tp, sym_config, /*verbose=*/false);
      if (sym_result.error == profiling::SymbolizerError::kOk &&
          !sym_result.symbols.empty()) {
        std::unique_ptr<uint8_t[]> buf(new uint8_t[sym_result.symbols.size()]);
        memcpy(buf.get(), sym_result.symbols.data(), sym_result.symbols.size());
        auto parse_status =
            tp->Parse(std::move(buf), sym_result.symbols.size());
        if (!parse_status.ok()) {
          PERFETTO_DFATAL_OR_ELOG("Failed to parse: %s",
                                  parse_status.message().c_str());
        }
      }
    } else {
      PERFETTO_ELOG("Skipping symbolization for non-proto trace");
    }
  }

  auto maybe_map = profiling::GetPerfettoProguardMapPath();
  if (!maybe_map.empty()) {
    if (is_proto_trace) {
      tp->Flush();
      profiling::ReadProguardMapsToDeobfuscationPackets(
          maybe_map, [tp](const std::string& trace_proto) {
            std::unique_ptr<uint8_t[]> buf(new uint8_t[trace_proto.size()]);
            memcpy(buf.get(), trace_proto.data(), trace_proto.size());
            auto parse_status = tp->Parse(std::move(buf), trace_proto.size());
            if (!parse_status.ok()) {
              PERFETTO_DFATAL_OR_ELOG("Failed to parse: %s",
                                      parse_status.message().c_str());
              return;
            }
          });
    } else {
      PERFETTO_ELOG("Skipping deobfuscation for non-proto trace");
    }
  }

  RETURN_IF_ERROR(tp->NotifyEndOfFile());
  base::TimeNanos t_load = base::GetWallTimeNs() - t_load_start;

  double t_load_s = static_cast<double>(t_load.count()) / 1E9;
  PERFETTO_ILOG("Trace loaded: %.2f MB in %.2fs (%.1f MB/s)", size_mb, t_load_s,
                size_mb / t_load_s);

  RETURN_IF_ERROR(PrintStats(tp));
  return t_load;
}

}  // namespace perfetto::trace_processor::shell
