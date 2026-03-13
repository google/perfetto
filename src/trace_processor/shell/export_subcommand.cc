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

#include "src/trace_processor/shell/export_subcommand.h"

#include <cstring>
#include <memory>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/trace_processor/trace_processor_shell.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/shell/common_flags.h"
#include "src/trace_processor/shell/shell_utils.h"

namespace perfetto::trace_processor::shell {

const char* ExportSubcommand::name() const {
  return "export";
}

const char* ExportSubcommand::description() const {
  return "Export trace to a database file.";
}

void ExportSubcommand::PrintUsage(const char* argv0) {
  PERFETTO_ELOG(R"(
Export trace to a database file.

Usage: %s export <format> -o FILE trace_file

Formats: sqlite
)",
                argv0);
}

static const option kExportLongOptions[] = {
    {"output", required_argument, nullptr, 'o'},
    GLOBAL_LONG_OPTIONS{nullptr, 0, nullptr, 0}};

const option* ExportSubcommand::GetLongOptions() const {
  return kExportLongOptions;
}

int ExportSubcommand::Run(const SubcommandContext& ctx, int argc, char** argv) {
  GlobalOptions global;
  std::string output_path;

  optind = 1;
  for (;;) {
    int option = getopt_long(argc, argv, "o:m:h", kExportLongOptions, nullptr);
    if (option == -1)
      break;
    if (HandleGlobalOption(option, optarg, global))
      continue;
    if (option == 'o') {
      output_path = optarg;
      continue;
    }
    PrintUsage(argv[0]);
    return option == 'h' ? 0 : 1;
  }

  // First positional arg is the format.
  if (optind >= argc) {
    PERFETTO_ELOG("export: must specify format (sqlite)");
    PrintUsage(argv[0]);
    return 1;
  }
  const char* format = argv[optind++];

  if (strcmp(format, "sqlite") != 0) {
    PERFETTO_ELOG("export: unknown format '%s' (expected sqlite)", format);
    return 1;
  }

  if (output_path.empty()) {
    PERFETTO_ELOG("export: -o FILE is required");
    return 1;
  }

  // Trace file is the last positional argument.
  if (optind == argc - 1 && argv[optind]) {
    global.trace_file = argv[optind];
  } else {
    PERFETTO_ELOG("export: trace file is required");
    return 1;
  }

  auto config = BuildConfig(global, ctx.platform);
  auto tp_or = SetupTraceProcessor(global, config, ctx.platform);
  if (!tp_or.ok()) {
    PERFETTO_ELOG("%s", tp_or.status().c_message());
    return 1;
  }
  auto tp = std::move(*tp_or);

  auto t_load_or = LoadTraceFile(tp.get(), ctx.platform, global.trace_file);
  if (!t_load_or.ok()) {
    PERFETTO_ELOG("%s", t_load_or.status().c_message());
    return 1;
  }

  auto status = ExportTraceToDatabase(tp.get(), output_path);
  if (!status.ok()) {
    PERFETTO_ELOG("%s", status.c_message());
    return 1;
  }

  return 0;
}

}  // namespace perfetto::trace_processor::shell
