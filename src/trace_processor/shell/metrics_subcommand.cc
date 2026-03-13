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

#include "src/trace_processor/shell/metrics_subcommand.h"

#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/trace_processor/trace_processor_shell.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/shell/common_flags.h"
#include "src/trace_processor/shell/interactive.h"
#include "src/trace_processor/shell/metatrace.h"
#include "src/trace_processor/shell/metrics.h"
#include "src/trace_processor/shell/query.h"

#include <google/protobuf/descriptor.h>

namespace perfetto::trace_processor::shell {

const char* MetricsSubcommand::name() const {
  return "metrics";
}

const char* MetricsSubcommand::description() const {
  return "Run v1 metrics (deprecated).";
}

void MetricsSubcommand::PrintUsage(const char* argv0) {
  PERFETTO_ELOG(R"(
Run v1 metrics (deprecated).

Usage: %s metrics [flags] trace_file

Flags:
  --run NAMES            Comma-separated metric names.
  --pre FILE             SQL file before metrics.
  --output [binary|text|json]
  --extension DISK@VIRTUAL
  --post-query FILE      SQL file after metrics.
)",
                argv0);
}

enum MetricsLocalOption {
  OPT_RUN = 500,
  OPT_PRE,
  OPT_OUTPUT,
  OPT_EXTENSION,
  OPT_POST_QUERY,
};

static const option kMetricsLongOptions[] = {
    {"run", required_argument, nullptr, OPT_RUN},
    {"pre", required_argument, nullptr, OPT_PRE},
    {"output", required_argument, nullptr, OPT_OUTPUT},
    {"extension", required_argument, nullptr, OPT_EXTENSION},
    {"post-query", required_argument, nullptr, OPT_POST_QUERY},
    {"interactive", no_argument, nullptr, 'i'},
    GLOBAL_LONG_OPTIONS{nullptr, 0, nullptr, 0}};

const option* MetricsSubcommand::GetLongOptions() const {
  return kMetricsLongOptions;
}

int MetricsSubcommand::Run(const SubcommandContext& ctx,
                           int argc,
                           char** argv) {
  GlobalOptions global;
  std::string metric_names;
  std::string pre_path;
  std::string metric_output;
  std::vector<std::string> raw_extensions;
  std::string post_query_path;
  bool interactive = false;

  optind = 1;
  for (;;) {
    int option = getopt_long(argc, argv, "im:h", kMetricsLongOptions, nullptr);
    if (option == -1)
      break;
    if (HandleGlobalOption(option, optarg, global))
      continue;
    if (option == OPT_RUN) {
      metric_names = optarg;
      continue;
    }
    if (option == OPT_PRE) {
      pre_path = optarg;
      continue;
    }
    if (option == OPT_OUTPUT) {
      metric_output = optarg;
      continue;
    }
    if (option == OPT_EXTENSION) {
      raw_extensions.emplace_back(optarg);
      continue;
    }
    if (option == OPT_POST_QUERY) {
      post_query_path = optarg;
      continue;
    }
    if (option == 'i') {
      interactive = true;
      continue;
    }
    PrintUsage(argv[0]);
    return option == 'h' ? 0 : 1;
  }

  if (metric_names.empty()) {
    PERFETTO_ELOG("metrics: --run is required");
    PrintUsage(argv[0]);
    return 1;
  }

  if (optind == argc - 1 && argv[optind]) {
    global.trace_file = argv[optind];
  } else {
    PERFETTO_ELOG("metrics: trace file is required");
    return 1;
  }

  // Parse metric extensions.
  std::vector<MetricExtension> metric_extensions;
  auto ext_status =
      ParseMetricExtensionPaths(global.dev, raw_extensions, metric_extensions);
  if (!ext_status.ok()) {
    PERFETTO_ELOG("%s", ext_status.c_message());
    return 1;
  }

  auto config = BuildConfig(global, ctx.platform);
  auto tp_or = SetupTraceProcessor(global, config, ctx.platform);
  if (!tp_or.ok()) {
    PERFETTO_ELOG("%s", tp_or.status().c_message());
    return 1;
  }
  auto tp = std::move(*tp_or);

  // Descriptor pool for metric output.
  google::protobuf::DescriptorPool pool(
      google::protobuf::DescriptorPool::generated_pool());
  auto status = PopulateDescriptorPool(pool, metric_extensions);
  if (!status.ok()) {
    PERFETTO_ELOG("%s", status.c_message());
    return 1;
  }
  for (const auto& extension : metric_extensions) {
    status = LoadMetricExtension(tp.get(), extension, pool);
    if (!status.ok()) {
      PERFETTO_ELOG("%s", status.c_message());
      return 1;
    }
  }

  auto t_load_or = LoadTraceFile(tp.get(), ctx.platform, global.trace_file);
  if (!t_load_or.ok()) {
    PERFETTO_ELOG("%s", t_load_or.status().c_message());
    return 1;
  }

  // Pre-metrics query.
  if (!pre_path.empty()) {
    status = RunQueriesFromFile(tp.get(), pre_path, false);
    if (!status.ok()) {
      PERFETTO_ELOG("%s", status.c_message());
      return 1;
    }
  }

  // Load and run metrics.
  std::vector<MetricNameAndPath> metrics;
  status = LoadMetrics(tp.get(), metric_names, pool, metrics);
  if (!status.ok()) {
    PERFETTO_ELOG("%s", status.c_message());
    return 1;
  }

  MetricV1OutputFormat format = MetricV1OutputFormat::kTextProto;
  if (metric_output == "binary") {
    format = MetricV1OutputFormat::kBinaryProto;
  } else if (metric_output == "json") {
    format = MetricV1OutputFormat::kJson;
  }

  status = RunMetrics(tp.get(), metrics, format);
  if (!status.ok()) {
    PERFETTO_ELOG("%s", status.c_message());
    return 1;
  }

  // Post-query.
  if (!post_query_path.empty()) {
    status = RunQueriesFromFile(tp.get(), post_query_path, true);
    if (!status.ok()) {
      PERFETTO_ELOG("%s", status.c_message());
      return 1;
    }
  }

  if (interactive) {
    status = StartInteractiveShell(
        tp.get(),
        InteractiveOptions{20u, format, metric_extensions, metrics, &pool});
    if (!status.ok()) {
      PERFETTO_ELOG("%s", status.c_message());
      return 1;
    }
  }

  status = MaybeWriteMetatrace(tp.get(), global.metatrace_path);
  if (!status.ok()) {
    PERFETTO_ELOG("%s", status.c_message());
    return 1;
  }
  return 0;
}

}  // namespace perfetto::trace_processor::shell
