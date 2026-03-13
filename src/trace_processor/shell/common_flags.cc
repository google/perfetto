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
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/trace_processor/trace_processor_shell.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/metatrace_config.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/shell/metatrace.h"
#include "src/trace_processor/shell/shell_utils.h"
#include "src/trace_processor/shell/sql_packages.h"

namespace perfetto::trace_processor::shell {

bool HandleGlobalOption(int option, const char* optarg, GlobalOptions& opts) {
  switch (option) {
    case 'm':
      opts.metatrace_path = optarg;
      return true;
    case OPT_GLOBAL_FULL_SORT:
      opts.force_full_sort = true;
      return true;
    case OPT_GLOBAL_NO_FTRACE_RAW:
      opts.no_ftrace_raw = true;
      return true;
    case OPT_GLOBAL_ANALYZE_TRACE_PROTO_CONTENT:
      opts.analyze_trace_proto_content = true;
      return true;
    case OPT_GLOBAL_CROP_TRACK_EVENTS:
      opts.crop_track_events = true;
      return true;
    case OPT_GLOBAL_DEV:
      opts.dev = true;
      return true;
    case OPT_GLOBAL_DEV_FLAG:
      opts.dev_flags.emplace_back(optarg);
      return true;
    case OPT_GLOBAL_EXTRA_CHECKS:
      opts.extra_checks = true;
      return true;
    case OPT_GLOBAL_ADD_SQL_PACKAGE:
      opts.sql_package_paths.emplace_back(optarg);
      return true;
    case OPT_GLOBAL_OVERRIDE_SQL_PACKAGE:
      opts.override_sql_package_paths.emplace_back(optarg);
      return true;
    case OPT_GLOBAL_OVERRIDE_STDLIB:
      opts.override_stdlib_path = optarg;
      return true;
    case OPT_GLOBAL_REGISTER_FILES_DIR:
      opts.register_files_dir = optarg;
      return true;
    case OPT_GLOBAL_METATRACE_BUFFER_CAPACITY:
      opts.metatrace_buffer_capacity = static_cast<size_t>(atoi(optarg));
      return true;
    case OPT_GLOBAL_METATRACE_CATEGORIES:
      opts.metatrace_categories = ParseMetatraceCategories(optarg);
      return true;
    default:
      return false;
  }
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
  RETURN_IF_ERROR(tp->NotifyEndOfFile());
  base::TimeNanos t_load = base::GetWallTimeNs() - t_load_start;

  double t_load_s = static_cast<double>(t_load.count()) / 1E9;
  PERFETTO_ILOG("Trace loaded: %.2f MB in %.2fs (%.1f MB/s)", size_mb, t_load_s,
                size_mb / t_load_s);

  RETURN_IF_ERROR(PrintStats(tp));
  return t_load;
}

}  // namespace perfetto::trace_processor::shell
